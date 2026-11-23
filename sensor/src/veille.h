// Sleep set-point rule ("veille hors-gel").
//
// In Sleep mode the spa only heats during its filter cycles, up to the set point minus 11 °C (ML700 guide).
// With the lowest set point (26 °C) the water is kept around 15 °C. The rule:
//   going to Sleep   : set point -> vSleepT (26 °C), then mode Sleep
//   leaving Sleep    : mode Standard / Économie, then set point -> vWakeT (34 °C)
// vWakeT is updated with the current set point each time the module goes to Sleep.
// The same rule can follow mode changes made on the spa keypad (after the new mode has been stable 60 s).
// Every step is checked (set point read back, mode read back) and retried up to 3 times.

#define V_SETTLE_MS 5000UL          // the set-point display goes back to the water temperature after 3 s
#define V_WAIT_MS 60000UL           // give up a step that cannot start for this long (queue busy…)
#define V_FOLLOW_STABLE_MS 60000UL  // a mode changed on the keypad must be stable this long
#define V_FOLLOW_MAX_PER_DAY 6

bool vOn = true;
float vSleepT = 26.0;
float vWakeT = 34.0;
bool vFollow = true;
String vLast = "";
uint32_t vLastAt = 0;
bool vLastOk = true;

struct VStep {
  uint8_t kind;   // 1 set point, 2 mode
  float t;
  int m;
};
VStep vSteps[2];
uint8_t vN = 0, vI = 0, vPhase = 0, vTries = 0;
unsigned long vAt = 0;
String vWho = "";
int vObsMode = -1;                 // mode seen on the bus
unsigned long vObsSince = 0;
int vAppliedMode = -1;             // mode the rule was last applied for (-1: nothing yet, e.g. just booted)
int vFollowDay = -1;
uint8_t vFollowCount = 0;

const char* V_MODE[3] = {"Standard", "Économie", "Veille"};

void veilleSave() {
  prefs.putBool("v_on", vOn);
  prefs.putFloat("v_st", vSleepT);
  prefs.putFloat("v_wt", vWakeT);
  prefs.putBool("v_fol", vFollow);
}

void veilleLoad() {
  vOn = prefs.getBool("v_on", true);
  vSleepT = prefs.getFloat("v_st", 26.0);
  vWakeT = prefs.getFloat("v_wt", 34.0);
  vFollow = prefs.getBool("v_fol", true);
}

bool vTargetKnown() {
  return tubTargetTemp >= 26 && tubTargetTemp <= 40;
}

String vStepText(const VStep& s) {
  return s.kind == 1 ? "consigne " + String(s.t, 1) + " °C" : String("mode ") + V_MODE[s.m];
}

void vEnd(bool ok, const String& msg) {
  seqProc = false;
  vN = 0;
  vLast = msg;
  vLastAt = nowEpoch();
  vLastOk = ok;
  logAdd(ok ? 'S' : 'E', msg);
}

void vStart(const char* who, const VStep* steps, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) vSteps[i] = steps[i];
  vN = n;
  vI = 0;
  vPhase = 0;
  vTries = 0;
  vAt = millis();
  vWho = who;
  seqProc = true;
  String plan;
  for (uint8_t i = 0; i < n; i++) plan += (i ? " puis " : "") + vStepText(steps[i]);
  logAdd('W', vWho + " : " + plan);
}

// When the rule applies, (re)remember the set point to restore after Sleep
void vRememberWake() {
  if (vTargetKnown() && tubTargetTemp > vSleepT + 0.2) {
    vWakeT = tubTargetTemp;
    veilleSave();
  }
}

// Same as requestMode(), plus the set-point rule around the Sleep mode
String requestModeV(int m, const char* who) {
  if (!panelDetected) return "nopanel";
  if (m < 0 || m > 2) return "bad mode";
  if (otaTask || kbProc || seqProc) return "busy";
  if (!vOn) return requestMode(m, who);
  int cur = modeIdx;
  if (cur < 0) return "mode unknown";
  if (!sendBuffer.isEmpty()) return "busy";
  if (m == MODE_IDX_SLP) {
    if (cur != MODE_IDX_SLP) {
      vRememberWake();
      VStep st[2] = {{1, vSleepT, 0}, {2, 0, MODE_IDX_SLP}};
      vStart(who, st, 2);
      return "ok";
    }
    if (vTargetKnown() && fabs(tubTargetTemp - vSleepT) > 0.2) {   // already asleep, set point not lowered
      VStep st[1] = {{1, vSleepT, 0}};
      vStart(who, st, 1);
      return "ok";
    }
    return "no change";
  }
  if (cur == MODE_IDX_SLP) {
    VStep st[2] = {{2, 0, m}, {1, vWakeT, 0}};
    vStart(who, st, vWakeT >= 26 ? 2 : 1);
    return "ok";
  }
  return requestMode(m, who);
}

void vNext() {
  logAdd('S', vWho + " : " + vStepText(vSteps[vI]) + " OK");
  if (vSteps[vI].kind == 2) vAppliedMode = vSteps[vI].m;
  vI++;
  vTries = 0;
  vPhase = 0;
  vAt = millis();
  if (vI >= vN) {
    String done;
    for (uint8_t i = 0; i < vN; i++) done += (i ? ", " : "") + vStepText(vSteps[i]);
    vEnd(true, vWho + " : " + done);
  }
}

void vFollowCheck() {
  int m = modeIdx;
  if (m != vObsMode) {
    vObsMode = m;
    vObsSince = millis();
    return;
  }
  if (m < 0 || millis() - vObsSince < V_FOLLOW_STABLE_MS) return;
  if (vAppliedMode < 0) {          // first stable mode after boot: nothing to do
    vAppliedMode = m;
    return;
  }
  if (m == vAppliedMode) return;
  int prev = vAppliedMode;
  vAppliedMode = m;                // whatever happens, react once per change
  if (!vOn || !vFollow || !vTargetKnown()) return;
  if (!panelDetected || kbProc || otaTask || !sendBuffer.isEmpty() || faMenu != "00") return;
  uint32_t now = nowEpoch();
  if (now) {
    time_t t = now;
    struct tm lt;
    localtime_r(&t, &lt);
    if (lt.tm_yday != vFollowDay) {
      vFollowDay = lt.tm_yday;
      vFollowCount = 0;
    }
  }
  if (vFollowCount >= V_FOLLOW_MAX_PER_DAY) return;
  if (m == MODE_IDX_SLP && fabs(tubTargetTemp - vSleepT) > 0.2) {
    vFollowCount++;
    vRememberWake();
    VStep st[1] = {{1, vSleepT, 0}};
    vStart("Veille choisie au clavier", st, 1);
  } else if (prev == MODE_IDX_SLP && m != MODE_IDX_SLP && vWakeT >= 26 && fabs(tubTargetTemp - vWakeT) > 0.2) {
    vFollowCount++;
    VStep st[1] = {{1, vWakeT, 0}};
    vStart("Sortie de veille au clavier", st, 1);
  }
}

void veilleTick() {
  if (!seqProc) {
    vFollowCheck();
    return;
  }
  VStep& s = vSteps[vI];
  switch (vPhase) {
    case 0: {   // start the step
      if (!sendBuffer.isEmpty() || kbProc || otaTask) {
        if (millis() - vAt > V_WAIT_MS) vEnd(false, vWho + " : " + vStepText(s) + " impossible (file de commandes occupée)");
        return;
      }
      if (s.kind == 1) {
        if (vTargetKnown() && fabs(tubTargetTemp - s.t) < 0.25) {
          vNext();
          return;
        }
        setTargetTemperature(s.t);   // if the set point is unknown, this only reads it (one Warm press)
      } else {
        String r = requestMode(s.m, vWho.c_str());
        if (r == "no change") {
          vNext();
          return;
        }
        if (r != "ok") {
          if ((r == "busy" || r == "mode unknown") && millis() - vAt < V_WAIT_MS) return;
          vEnd(false, vWho + " : " + vStepText(s) + " impossible (" + r + ")");
          return;
        }
      }
      vPhase = 1;
      return;
    }
    case 1:     // keys being sent
      if (!sendBuffer.isEmpty()) return;
      vPhase = 2;
      vAt = millis();
      return;
    case 2: {   // check the result
      if (millis() - vAt < V_SETTLE_MS) return;
      bool ok = s.kind == 1 ? (vTargetKnown() && fabs(tubTargetTemp - s.t) < 0.25) : modeIdx == s.m;
      if (ok) {
        vNext();
      } else if (++vTries < 4) {
        logAdd('E', vWho + " : " + vStepText(s) + " pas encore obtenu (consigne lue " + String(tubTargetTemp, 1) +
                        ", mode " + String(modeIdx) + "), nouvel essai");
        vPhase = 0;
        vAt = millis();
      } else {
        vEnd(false, vWho + " : " + vStepText(s) + " non obtenu après 4 essais");
      }
      return;
    }
  }
}

// GET /api/veille
void handleApiVeilleGet() {
  String j = "{\"on\":" + String(vOn ? 1 : 0) + ",\"st\":" + String(vSleepT, 1) + ",\"wt\":" + String(vWakeT, 1) +
             ",\"follow\":" + String(vFollow ? 1 : 0) + ",\"job\":" + String(seqProc ? 1 : 0) + ",\"last\":\"" +
             jsonEscape(vLast) + "\",\"lastAt\":" + String(vLastAt) + ",\"lastOk\":" + String(vLastOk ? 1 : 0) + "}";
  webserver.send(200, "application/json", j);
}

// POST /api/veille?on=1&st=26&wt=34&follow=1
void handleApiVeillePost() {
  float st = webserver.hasArg("st") ? webserver.arg("st").toFloat() : vSleepT;
  float wt = webserver.hasArg("wt") ? webserver.arg("wt").toFloat() : vWakeT;
  st = round(st * 2) / 2.0;
  wt = round(wt * 2) / 2.0;
  if (st < 26 || st > 40 || wt < 26 || wt > 40) {
    sendApiResult(false, "out of range");
    return;
  }
  if (webserver.hasArg("on")) vOn = webserver.arg("on") == "1";
  if (webserver.hasArg("follow")) vFollow = webserver.arg("follow") == "1";
  vSleepT = st;
  vWakeT = wt;
  veilleSave();
  logAdd('W', String("Veille hors-gel ") + (vOn ? "active" : "désactivée") + " : consigne en veille " + String(st, 1) +
                  " °C, au retour " + String(wt, 1) + " °C" + (vFollow ? ", suit aussi le clavier" : ""));
  sendApiResult(true, "ok");
}
