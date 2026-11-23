// Spa clock setting and filter cycle read-out, by replaying the ML700 key sequences.
//
// The module has the real time (Internet, French time zone with the summer/winter time changes, weblog.h).
// The spa has its own clock, used for its filter cycles. ML700 user guide 40520-99, page 2:
//   Time, Mode/Prog            -> time setting, hour (Warm / Cool)
//   Mode/Prog                  -> minutes (Warm / Cool, 1 min per press)
//   Time                       -> saves and leaves (Mode/Prog would go on to the filter cycles)
// Page 3: Time, Mode/Prog, Mode/Prog, Mode/Prog -> filter 1 start hour, then minutes (5 min per press),
// filter 1 end, filter 2 start, filter 2 end; Mode/Prog after the last one saves; Time saves and leaves.
//
// Safety: every key waits for the controller's echo before the next one. Before each Warm / Cool the
// display must not show a temperature (or it would change the set temperature) and the mode menu (4c)
// must not be open (Cool would change the heating mode). Otherwise the sequence stops and the menu
// closes by itself after its timeout. The filter read-out only presses Time and Mode/Prog: values are
// shown, never changed. Writing the filter cycles needs the display format of these menus, which this
// read-out records (journal, page).

#define CK_SETTLE_MS 700UL          // after an accepted key, wait for the display to follow
#define CK_KEY_TIMEOUT_MS 15000UL   // a key that never leaves the queue
#define CK_PRESS_S 1.2f             // average time per key, to aim at the time when the setting ends
#define CK_AUTO_EVERY_MS 10000UL
#define CK_AUTO_MAX_PER_DAY 3
#define CK_AUTO_BACKOFF_MS (2UL * 3600UL * 1000UL)
#define CK_FR_MAX 20                // read-out steps kept

enum CkJob { CJ_NONE = 0, CJ_SYNC = 1, CJ_FREAD = 2 };

struct CkStepRec {
  char key;        // 'T' Time, 'M' Mode/Prog
  String menu;
  String disp;
  String hex;
};

struct {
  uint8_t job = CJ_NONE;
  uint8_t step = 0;
  bool waitKey = false;
  char keyName = ' ';
  uint32_t ackBefore = 0;
  unsigned long t = 0;           // when the key was queued
  unsigned long settleAt = 0;    // when the wait after the key started
  unsigned long extraWait = 0;   // additional wait before the next step
  const char* who = "";
  // clock setting
  int h0 = -1, m0 = -1;          // spa clock when the sequence started
  int hLeft = 0, mLeft = 0;      // presses still to do (sign: + Warm, - Cool)
  uint8_t attempt = 0;
  bool wrap = true;              // take the shortest way (23 -> 0); retry without it if that failed
  // filter read-out
  uint8_t modes = 0;
} ck;

bool clockAuto = true;           // set the spa clock by itself when it drifts (summer / winter time)
bool clockOk = false;            // one setting already succeeded: the key sequence works on this spa
String ckLast = "";
uint32_t ckLastAt = 0;
bool ckLastOk = false;
unsigned long ckAutoAt = 0;
unsigned long ckBackoffSince = 0;
bool ckBackoff = false;
int ckAutoDay = -1;
uint8_t ckAutoCount = 0;

CkStepRec ckFr[CK_FR_MAX];
uint8_t ckFrN = 0;
uint32_t ckFrAt = 0;
bool ckFrDone = false;

String ckDispText() {
  return cleanAscii(faHex.substring(4, 12));
}

// the display shows the water temperature (or "----"): the panel is in its normal state
bool ckStatusDisp() {
  if (faDisp.length() < 4) return false;
  char c = faDisp[3];
  return c == 'C' || c == 'F' || c == '-';
}

bool ckMenuOpen() {
  return millis() - faAt < 2000 && !ckStatusDisp() && faMenu != "4c";
}

bool ckIdle() {
  return panelDetected && !otaTask && sendBuffer.isEmpty() && faMenu == "00" && ckStatusDisp() && millis() - faAt < 2000;
}

void clockSave() {
  prefs.putBool("clkauto", clockAuto);
  prefs.putBool("clkok", clockOk);
}

void clockLoad() {
  clockAuto = prefs.getBool("clkauto", true);
  clockOk = prefs.getBool("clkok", false);
}

// local time from the Internet clock; false if not known
bool ckLocal(uint32_t epoch, struct tm& lt) {
  if (!epoch) return false;
  time_t t = epoch;
  localtime_r(&t, &lt);
  return true;
}

// spa clock minus real time, in minutes (-720..720); 9999 if one of them is unknown
int ckDiff() {
  struct tm lt;
  if (spaHour < 0 || !ckLocal(nowEpoch(), lt)) return 9999;
  int d = (spaHour * 60 + spaMin) - (lt.tm_hour * 60 + lt.tm_min);
  if (d > 720) d -= 1440;
  if (d < -720) d += 1440;
  return d;
}

int ckDist(int from, int to, int mod, bool wrap) {
  if (!wrap) return to - from;
  int d = ((to - from) % mod + mod) % mod;
  if (d > mod / 2) d -= mod;
  return d;
}

void ckEnd(bool ok, const String& msg) {
  ck.job = CJ_NONE;
  ck.waitKey = false;
  kbProc = false;
  ckLast = msg;
  ckLastAt = nowEpoch();
  ckLastOk = ok;
  logAdd(ok ? 'S' : 'E', msg);
}

void ckPress(const String& code, char name) {
  ck.ackBefore = cmdAckCount;
  ck.waitKey = true;
  ck.keyName = name;
  ck.t = millis();
  ck.extraWait = 0;
  sendBuffer.enqueue(code);
}

const char* ckKeyFr(char k) {
  switch (k) {
    case 'T': return "Time";
    case 'M': return "Mode/Prog";
    case '+': return "Warm";
    case '-': return "Cool";
    default: return "?";
  }
}

String ckWhy() {
  return "menu=" + faMenu + ", affichage='" + ckDispText() + "'";
}

// ---- clock setting ----

bool ckStartSync(const char* who, String& why) {
  if (COMMAND_TIME.length() == 0 || COMMAND_CHANGE_MODE.length() == 0 || COMMAND_UP.length() == 0 ||
      COMMAND_DOWN.length() == 0) {
    why = "nokey";
    return false;
  }
  if (!panelDetected) { why = "nopanel"; return false; }
  if (!nowEpoch()) { why = "notime"; return false; }
  if (spaHour < 0) { why = "nospatime"; return false; }
  if (!ckIdle()) { why = "busy"; return false; }
  ck.who = who;
  ck.h0 = spaHour;
  ck.m0 = spaMin;
  ck.job = CJ_SYNC;
  kbProc = true;
  logAdd('S', String("Mise à l'heure du spa (") + who + ") : heure du spa " + timeString + ", écart " +
                  String(ckDiff()) + " min" + (ck.wrap ? "" : ", sans passer par minuit"));
  ck.step = 1;
  ckPress(COMMAND_TIME, 'T');
  return true;
}

void ckPlanHours() {
  // aim at the time the setting will end: about one key per CK_PRESS_S
  uint32_t now = nowEpoch();
  struct tm lt;
  float est = 6;
  int dh = 0, dm = 0;
  for (int k = 0; k < 3; k++) {
    ckLocal(now + (uint32_t)est, lt);
    dh = ckDist(ck.h0, lt.tm_hour, 24, ck.wrap);
    dm = ckDist(ck.m0, lt.tm_min, 60, ck.wrap);
    est = (abs(dh) + abs(dm) + 3) * CK_PRESS_S + 2;
  }
  ck.hLeft = dh;
}

void ckPlanMinutes() {
  uint32_t now = nowEpoch();
  struct tm lt;
  float est = 4;
  int dm = 0;
  for (int k = 0; k < 3; k++) {
    ckLocal(now + (uint32_t)est, lt);
    dm = ckDist(ck.m0, lt.tm_min, 60, ck.wrap);
    est = (abs(dm) + 1) * CK_PRESS_S + 2;
  }
  ck.mLeft = dm;
}

void ckStepSync() {
  switch (ck.step) {
    case 0: {  // retry: start again from the normal display
      String why;
      if (!ckStartSync(ck.who, why)) ckEnd(false, "Mise à l'heure du spa : nouvel essai impossible (" + why + ")");
      return;
    }
    case 1:    // Time accepted
      ck.step = 2;
      ckPress(COMMAND_CHANGE_MODE, 'M');
      return;
    case 2:    // Time, Mode/Prog: the hour should now be editable
      if (!ckMenuOpen()) {
        ckEnd(false, "Mise à l'heure du spa arrêtée : le réglage de l'heure ne s'est pas ouvert (" + ckWhy() + ")");
        return;
      }
      logAdd('S', "Réglage de l'heure ouvert : " + ckWhy());
      ckPlanHours();
      ck.step = 3;
      // fall through
    case 3:
      if (ck.hLeft != 0) {
        if (!ckMenuOpen()) {
          ckEnd(false, "Mise à l'heure du spa arrêtée pendant les heures : menu fermé (" + ckWhy() + ")");
          return;
        }
        bool up = ck.hLeft > 0;
        ck.hLeft += up ? -1 : 1;
        ckPress(up ? COMMAND_UP : COMMAND_DOWN, up ? '+' : '-');
        return;
      }
      ck.step = 4;
      ckPress(COMMAND_CHANGE_MODE, 'M');   // hour entered, minutes next
      return;
    case 4:
      if (!ckMenuOpen()) {
        ckEnd(false, "Mise à l'heure du spa arrêtée : le réglage des minutes ne s'est pas ouvert (" + ckWhy() + ")");
        return;
      }
      logAdd('S', "Réglage des minutes : " + ckWhy());
      ckPlanMinutes();
      ck.step = 5;
      // fall through
    case 5:
      if (ck.mLeft != 0) {
        if (!ckMenuOpen()) {
          ckEnd(false, "Mise à l'heure du spa arrêtée pendant les minutes : menu fermé (" + ckWhy() + ")");
          return;
        }
        bool up = ck.mLeft > 0;
        ck.mLeft += up ? -1 : 1;
        ckPress(up ? COMMAND_UP : COMMAND_DOWN, up ? '+' : '-');
        return;
      }
      ck.step = 6;
      ckPress(COMMAND_TIME, 'T');          // save and leave
      return;
    case 6:
      ck.step = 7;
      ck.settleAt = millis();
      ck.extraWait = 2500;                 // let the spa clock settle
      return;
    case 7: {
      int d = ckDiff();
      if (faMenu != "00") logAdd('E', "Après la mise à l'heure, le menu n'est pas refermé (" + ckWhy() + ")");
      if (d != 9999 && abs(d) <= 1) {
        bool first = !clockOk;
        clockOk = true;
        ck.wrap = true;
        ck.attempt = 0;
        clockSave();
        ckEnd(true, "Spa mis à l'heure : " + timeString + (first ? " (première mise à l'heure réussie, réglage automatique possible)" : ""));
        return;
      }
      if (ck.attempt == 0) {
        ck.attempt = 1;
        ck.wrap = !ck.wrap;                // maybe the hours or minutes do not go round (23 -> 0)
        logAdd('E', "Heure du spa encore décalée de " + String(d) + " min, nouvel essai dans 5 s");
        ck.step = 0;
        ck.settleAt = millis();
        ck.extraWait = 5000;
        return;
      }
      ck.attempt = 0;
      ck.wrap = true;
      ckEnd(false, "Mise à l'heure du spa : écart restant " + String(d) + " min après deux essais. Vérifiez les codes Time, Mode, Warm et Cool, puis envoyez le journal.");
      return;
    }
  }
}

// ---- filter cycle read-out (Time, then Mode/Prog only: nothing is changed) ----

void ckRecord(char key) {
  if (ckFrN >= CK_FR_MAX) return;
  CkStepRec& r = ckFr[ckFrN++];
  r.key = key;
  r.menu = faMenu;
  r.disp = ckDispText();
  r.hex = faHex.substring(0, 46);
  logAdd('S', "Relevé filtration " + String(ckFrN) + " après " + ckKeyFr(key) + " : menu=" + r.menu + ", affichage='" +
                  r.disp + "' | " + faHex.substring(4, 46));
}

bool ckStartFilterRead(String& why) {
  if (COMMAND_TIME.length() == 0 || COMMAND_CHANGE_MODE.length() == 0) { why = "nokey"; return false; }
  if (!panelDetected) { why = "nopanel"; return false; }
  if (!ckIdle()) { why = "busy"; return false; }
  ckFrN = 0;
  ckFrDone = false;
  ckFrAt = nowEpoch();
  ck.modes = 0;
  ck.job = CJ_FREAD;
  kbProc = true;
  logAdd('S', "Relevé des cycles de filtration (Time puis Mode/Prog, aucune valeur modifiée)");
  ck.step = 1;
  ckPress(COMMAND_TIME, 'T');
  return true;
}

void ckStepFilterRead() {
  switch (ck.step) {
    case 1:          // after Time
      ckRecord('T');
      ck.step = 2;
      ck.modes = 1;
      ckPress(COMMAND_CHANGE_MODE, 'M');
      return;
    case 2:          // after each Mode/Prog
      ckRecord('M');
      if (faMenu == "4c") {
        ckFrDone = true;
        ckEnd(false, "Relevé arrêté : le menu des modes s'est ouvert au lieu de l'heure (il se referme seul, sans changement)");
        return;
      }
      if (ck.modes >= 3 && faMenu == "00" && ckStatusDisp()) {
        ckFrDone = true;
        ckEnd(true, "Relevé des cycles de filtration terminé (" + String(ck.modes) + " appuis sur Mode/Prog)");
        return;
      }
      if (ck.modes >= 14 || ckFrN >= CK_FR_MAX - 1) {
        ck.step = 3;
        ckPress(COMMAND_TIME, 'T');  // leave (values unchanged)
        return;
      }
      ck.modes++;
      ckPress(COMMAND_CHANGE_MODE, 'M');
      return;
    case 3:
      ckRecord('T');
      ckFrDone = true;
      ckEnd(true, "Relevé des cycles de filtration terminé (sortie par Time après " + String(ck.modes) + " appuis sur Mode/Prog)");
      return;
  }
}

// ---- loop ----

void ckAutoCheck() {
  if (!clockAuto || !clockOk) return;
  if (millis() - ckAutoAt < CK_AUTO_EVERY_MS) return;
  ckAutoAt = millis();
  if (ckBackoff && millis() - ckBackoffSince < CK_AUTO_BACKOFF_MS) return;
  ckBackoff = false;
  struct tm lt;
  if (!ckLocal(nowEpoch(), lt)) return;
  if (lt.tm_yday != ckAutoDay) {
    ckAutoDay = lt.tm_yday;
    ckAutoCount = 0;
  }
  if (ckAutoCount >= CK_AUTO_MAX_PER_DAY) return;
  if (lt.tm_sec > 30) return;  // start early in the minute, so the minute does not change during the setting
  int d = ckDiff();
  if (d == 9999 || abs(d) < 2) return;
  // not while somebody is using the spa
  if (pump2Lvl > 0 || pump1Lvl == 2 || lightState) return;
  if (!ckIdle()) return;
  ckAutoCount++;
  logAdd('S', "Heure du spa décalée de " + String(d) + " min" + (abs(abs(d) - 60) <= 1 ? " (changement d'heure)" : "") +
                  " : mise à l'heure automatique");
  String why;
  if (!ckStartSync("automatique", why)) {
    ckBackoff = true;
    ckBackoffSince = millis();
  }
}

void clockTick() {
  if (ck.job == CJ_NONE) {
    ckAutoCheck();
    return;
  }
  if (ck.waitKey) {
    if (!sendBuffer.isEmpty()) {
      if (millis() - ck.t > CK_KEY_TIMEOUT_MS) {
        queueClear();
        ckEnd(false, String("Séquence au clavier arrêtée : touche ") + ckKeyFr(ck.keyName) + " sans réponse");
      }
      return;
    }
    ck.waitKey = false;
    if (cmdAckCount == ck.ackBefore) {
      ckEnd(false, String("Séquence au clavier arrêtée : touche ") + ckKeyFr(ck.keyName) + " non acceptée par le spa");
      ckBackoff = true;
      ckBackoffSince = millis();
      return;
    }
    ck.settleAt = millis();
    return;
  }
  // wait for the display to follow the key (and for a fresh frame)
  if (millis() - ck.settleAt < CK_SETTLE_MS + ck.extraWait) return;
  if ((long)(faAt - ck.settleAt) < 250) return;
  ck.extraWait = 0;
  if (ck.job == CJ_SYNC) {
    ckStepSync();
    if (ck.job == CJ_NONE && !ckLastOk) {
      ckBackoff = true;
      ckBackoffSince = millis();
    }
  } else if (ck.job == CJ_FREAD) {
    ckStepFilterRead();
  }
}

// ---- web API ----

// GET /api/clock
void handleApiClockGet() {
  int d = ckDiff();
  String j = "{\"auto\":" + String(clockAuto ? 1 : 0) + ",\"ok\":" + String(clockOk ? 1 : 0) + ",\"job\":\"" +
             String(ck.job == CJ_SYNC ? "sync" : ck.job == CJ_FREAD ? "fread" : "") + "\",\"now\":" + String(nowEpoch()) +
             ",\"spa\":\"" + (spaHour >= 0 ? jsonEscape(timeString) : String("")) + "\",\"diff\":" +
             (d == 9999 ? String("null") : String(d)) + ",\"last\":\"" + jsonEscape(ckLast) + "\",\"lastAt\":" +
             String(ckLastAt) + ",\"lastOk\":" + String(ckLastOk ? 1 : 0) + ",\"fr\":{\"at\":" + String(ckFrAt) +
             ",\"done\":" + String(ckFrDone ? 1 : 0) + ",\"steps\":[";
  for (int i = 0; i < ckFrN; i++) {
    if (i) j += ',';
    j += "[\"" + String(ckKeyFr(ckFr[i].key)) + "\",\"" + ckFr[i].menu + "\",\"" + jsonEscape(ckFr[i].disp) + "\",\"" +
         ckFr[i].hex + "\"]";
  }
  j += "]}}";
  webserver.send(200, "application/json", j);
}

// POST /api/clock?auto=0|1
void handleApiClockPost() {
  if (webserver.hasArg("auto")) {
    clockAuto = webserver.arg("auto") == "1";
    clockSave();
    logAdd('W', String("Mise à l'heure automatique du spa ") + (clockAuto ? "activée" : "désactivée"));
  }
  sendApiResult(true, "ok");
}

// POST /api/clock/sync
void handleApiClockSync() {
  String why;
  ck.attempt = 0;
  ck.wrap = true;
  bool ok = ckStartSync("depuis la page", why);
  sendApiResult(ok, ok ? String("started") : why);
}

// POST /api/clock/fread
void handleApiFilterRead() {
  String why;
  bool ok = ckStartFilterRead(why);
  sendApiResult(ok, ok ? String("started") : why);
}
