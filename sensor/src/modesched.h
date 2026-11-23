// Heating mode scheduler: the module switches the spa between Standard / Économie / Veille
// at set times (weekly entries) or once at a given date and time (one-shot entries).
// It uses the module clock (Internet time, French time zone). The spa's own filter cycles are not touched.

#define SCHED_W 8   // weekly entries
#define SCHED_O 6   // one-shot entries
#define SCHED_RETRY_MS (15UL * 60UL * 1000UL)   // keep trying a change for 15 min (mode unknown, queue busy…)

struct SchedW {
  uint8_t days;   // bit 0 = lundi … bit 6 = dimanche
  uint8_t hh;
  uint8_t mm;
  uint8_t mode;   // 0 Standard, 1 Économie, 2 Veille
  uint8_t on;
};
struct SchedO {
  uint32_t at;    // epoch (UTC)
  uint8_t mode;
};

bool schedOn = true;
SchedW schedW[SCHED_W];
uint8_t schedWN = 0;
SchedO schedO[SCHED_O];
uint8_t schedON = 0;
int schedPending = -1;
unsigned long schedPendingSince = 0;
int32_t schedLastKey = -1;
unsigned long schedTickAt = 0;

const char* MODE_FR[3] = {"Standard", "Économie", "Veille"};

// text form, also what the page sends: en=1&w=days-hh-mm-mode-on,…&o=epoch-mode,…
String schedToString() {
  String s = "en=" + String(schedOn ? 1 : 0) + "&w=";
  for (int i = 0; i < schedWN; i++) {
    if (i) s += ',';
    s += String(schedW[i].days) + "-" + String(schedW[i].hh) + "-" + String(schedW[i].mm) + "-" +
         String(schedW[i].mode) + "-" + String(schedW[i].on);
  }
  s += "&o=";
  for (int i = 0; i < schedON; i++) {
    if (i) s += ',';
    s += String(schedO[i].at) + "-" + String(schedO[i].mode);
  }
  return s;
}

// split "a-b-c" into numbers; returns how many were read
int schedNums(const String& item, long* out, int maxN) {
  int n = 0, pos = 0;
  while (n < maxN && pos <= (int)item.length()) {
    int end = item.indexOf('-', pos);
    if (end < 0) end = item.length();
    String part = item.substring(pos, end);
    if (part.length() == 0) return -1;
    for (size_t i = 0; i < part.length(); i++) {
      if (!isdigit((unsigned char)part[i])) return -1;
    }
    out[n++] = part.toInt();
    pos = end + 1;
    if (end == (int)item.length()) break;
  }
  return n;
}

bool schedParse(const String& en, const String& w, const String& o) {
  SchedW nw[SCHED_W];
  SchedO no[SCHED_O];
  uint8_t nwn = 0, non = 0;
  long v[5];
  int pos = 0;
  while (pos < (int)w.length()) {
    int end = w.indexOf(',', pos);
    if (end < 0) end = w.length();
    if (nwn >= SCHED_W) return false;
    if (schedNums(w.substring(pos, end), v, 5) != 5) return false;
    if (v[0] < 1 || v[0] > 127 || v[1] > 23 || v[2] > 59 || v[3] > 2 || v[4] > 1) return false;
    nw[nwn++] = {(uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4]};
    pos = end + 1;
  }
  pos = 0;
  while (pos < (int)o.length()) {
    int end = o.indexOf(',', pos);
    if (end < 0) end = o.length();
    if (non >= SCHED_O) return false;
    if (schedNums(o.substring(pos, end), v, 2) != 2) return false;
    if ((uint32_t)v[0] < CLOCK_VALID || v[1] > 2) return false;
    no[non++] = {(uint32_t)v[0], (uint8_t)v[1]};
    pos = end + 1;
  }
  schedOn = en != "0";
  memcpy(schedW, nw, sizeof(SchedW) * nwn);
  schedWN = nwn;
  memcpy(schedO, no, sizeof(SchedO) * non);
  schedON = non;
  return true;
}

String schedArg(const String& s, const char* name) {
  String key = String(name) + "=";
  int p = s.startsWith(key) ? 0 : s.indexOf("&" + key);
  if (p < 0) return "";
  if (p > 0) p++;
  p += key.length();
  int e = s.indexOf('&', p);
  return s.substring(p, e < 0 ? s.length() : e);
}

void schedSave() { prefs.putString("sched", schedToString()); }

void schedLoad() {
  String s = prefs.getString("sched", "");
  if (s.length() && !schedParse(schedArg(s, "en"), schedArg(s, "w"), schedArg(s, "o"))) {
    logAdd('E', "Programmation enregistrée illisible, ignorée");
  }
}

void schedFire(int mode, const String& why) {
  schedPending = mode;
  schedPendingSince = millis();
  logAdd('S', "Programmation (" + why + ") : passage en " + MODE_FR[mode]);
}

// next planned change after "now", for the page (0 if none)
uint32_t schedNext(uint32_t now, int* modeOut) {
  uint32_t best = 0;
  int bestMode = -1;
  for (int i = 0; schedOn && i < schedON; i++) {
    if (schedO[i].at > now && (!best || schedO[i].at < best)) {
      best = schedO[i].at;
      bestMode = schedO[i].mode;
    }
  }
  time_t t = now;
  struct tm base;
  localtime_r(&t, &base);
  for (int i = 0; schedOn && i < schedWN; i++) {
    if (!schedW[i].on) continue;
    for (int d = 0; d < 8; d++) {
      struct tm c = base;
      c.tm_mday += d;
      c.tm_hour = schedW[i].hh;
      c.tm_min = schedW[i].mm;
      c.tm_sec = 0;
      c.tm_isdst = -1;
      time_t e = mktime(&c);       // normalises the date
      struct tm chk;
      localtime_r(&e, &chk);
      int dow = (chk.tm_wday + 6) % 7;
      if ((uint32_t)e > now && (schedW[i].days & (1 << dow))) {
        if (!best || (uint32_t)e < best) {
          best = e;
          bestMode = schedW[i].mode;
        }
        break;
      }
    }
  }
  if (modeOut) *modeOut = bestMode;
  return best;
}

void schedTick() {
  if (millis() - schedTickAt < 5000) return;
  schedTickAt = millis();
  uint32_t now = nowEpoch();
  if (now && schedOn) {
    time_t t = now;
    struct tm lt;
    localtime_r(&t, &lt);
    int32_t key = lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min;
    if (key != schedLastKey) {
      bool first = schedLastKey < 0;    // just booted / clock just set: do not replay this minute twice
      schedLastKey = key;
      int dow = (lt.tm_wday + 6) % 7;
      for (int i = 0; !first && i < schedWN; i++) {
        if (schedW[i].on && (schedW[i].days & (1 << dow)) && schedW[i].hh == lt.tm_hour && schedW[i].mm == lt.tm_min) {
          schedFire(schedW[i].mode, "hebdomadaire");
        }
      }
    }
    bool changed = false;
    for (int i = 0; i < schedON;) {
      if (now >= schedO[i].at) {
        if (now - schedO[i].at < 6UL * 3600UL) {
          schedFire(schedO[i].mode, "ponctuelle");
        } else {
          logAdd('E', String("Programmation ponctuelle manquée (module éteint ?) : ") + MODE_FR[schedO[i].mode]);
        }
        for (int j = i; j < schedON - 1; j++) schedO[j] = schedO[j + 1];
        schedON--;
        changed = true;
      } else {
        i++;
      }
    }
    if (changed) schedSave();
  }
  if (schedPending >= 0) {
    String r = requestModeV(schedPending, "Programmation");
    if (r == "ok" || r == "no change") {
      schedPending = -1;
    } else if (millis() - schedPendingSince > SCHED_RETRY_MS) {
      logAdd('E', String("Programmation : passage en ") + MODE_FR[schedPending] + " impossible (" + r + ")");
      schedPending = -1;
    }
  }
}

// GET /api/sched
void handleApiSchedGet() {
  uint32_t now = nowEpoch();
  int nm = -1;
  uint32_t next = now ? schedNext(now, &nm) : 0;
  String j = "{\"en\":" + String(schedOn ? 1 : 0) + ",\"w\":[";
  for (int i = 0; i < schedWN; i++) {
    if (i) j += ',';
    j += "[" + String(schedW[i].days) + "," + String(schedW[i].hh) + "," + String(schedW[i].mm) + "," +
         String(schedW[i].mode) + "," + String(schedW[i].on) + "]";
  }
  j += "],\"o\":[";
  for (int i = 0; i < schedON; i++) {
    if (i) j += ',';
    j += "[" + String(schedO[i].at) + "," + String(schedO[i].mode) + "]";
  }
  j += "],\"now\":" + String(now) + ",\"spa\":\"" + jsonEscape(timeString) + "\",\"next\":" + String(next) +
       ",\"nextMode\":" + String(nm) + ",\"pending\":" + String(schedPending) + "}";
  webserver.send(200, "application/json", j);
}

// POST /api/sched?en=1&w=…&o=…
void handleApiSchedPost() {
  if (!schedParse(webserver.arg("en"), webserver.arg("w"), webserver.arg("o"))) {
    sendApiResult(false, "bad schedule");
    return;
  }
  schedSave();
  logAdd('W', "Programmation enregistrée : " + String(schedWN) + " horaire(s) hebdomadaire(s), " + String(schedON) +
                  " ponctuel(s)" + (schedOn ? "" : " (désactivée)"));
  sendApiResult(true, "ok");
}
