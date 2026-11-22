// Button codes of the topside panel, editable from the web page and stored in the module.
// A code is the 9-byte FB frame the panel sends, written as 18 hex characters ("fb06…").
// An empty code (pump 3 by default) means "unknown": the matching button is refused.

struct KeyDef {
  const char* id;
  const char* name;
  const char* def;
};

const KeyDef KEY_DEFS[K_COUNT] = {
  {"up",    "Warm (consigne +)",   DEF_COMMAND_UP},
  {"down",  "Cool (consigne −)",   DEF_COMMAND_DOWN},
  {"mode",  "Mode / Prog",         DEF_COMMAND_CHANGE_MODE},
  {"light", "Light (éclairage)",   DEF_COMMAND_LIGHT},
  {"jet1",  "Jets 1 (pompe 1)",    DEF_COMMAND_JET1},
  {"jet2",  "Jets 2 (pompe 2)",    DEF_COMMAND_JET2},
  {"jet3",  "Jets 3 (pompe 3)",    DEF_COMMAND_JET3},
  {"aux",   "Blower / Aux",        DEF_COMMAND_AUX},
  {"time",  "Time (heure)",        DEF_COMMAND_TIME},
};

String keyCode[K_COUNT] = {
  DEF_COMMAND_UP, DEF_COMMAND_DOWN, DEF_COMMAND_CHANGE_MODE, DEF_COMMAND_LIGHT,
  DEF_COMMAND_JET1, DEF_COMMAND_JET2, DEF_COMMAND_JET3, DEF_COMMAND_AUX, DEF_COMMAND_TIME,
};

int keyIndex(const String& id) {
  for (int i = 0; i < K_COUNT; i++) {
    if (id == KEY_DEFS[i].id) return i;
  }
  return -1;
}

bool keyValid(const String& h) {
  if (h.length() != 18 || !h.startsWith("fb06")) return false;
  for (size_t i = 0; i < h.length(); i++) {
    if (!isxdigit((unsigned char)h[i])) return false;
  }
  return true;
}

// stored as "id=hex;id=hex" (only the codes that differ from the default)
void keysSave() {
  String s;
  for (int i = 0; i < K_COUNT; i++) {
    if (keyCode[i] != KEY_DEFS[i].def) {
      s += KEY_DEFS[i].id;
      s += '=';
      s += keyCode[i];
      s += ';';
    }
  }
  prefs.putString("keys", s);
}

void keysLoad() {
  String s = prefs.getString("keys", "");
  int pos = 0;
  while (pos < (int)s.length()) {
    int end = s.indexOf(';', pos);
    if (end < 0) end = s.length();
    String item = s.substring(pos, end);
    int eq = item.indexOf('=');
    if (eq > 0) {
      int k = keyIndex(item.substring(0, eq));
      String h = item.substring(eq + 1);
      h.toLowerCase();
      if (k >= 0 && (h.length() == 0 || keyValid(h))) keyCode[k] = h;
    }
    pos = end + 1;
  }
}

// GET /api/keys -> {"keys":[{"id":"up","name":"…","code":"fb06…","def":"fb06…"},…]}
void handleApiKeysGet() {
  String j = "{\"keys\":[";
  for (int i = 0; i < K_COUNT; i++) {
    if (i) j += ',';
    j += "{\"id\":\"" + String(KEY_DEFS[i].id) + "\",\"name\":\"" + jsonEscape(KEY_DEFS[i].name) +
         "\",\"code\":\"" + keyCode[i] + "\",\"def\":\"" + String(KEY_DEFS[i].def) + "\"}";
  }
  j += "]}";
  webserver.send(200, "application/json", j);
}

// POST /api/keys?id=jet3&h=fb06…   (h empty = back to the default code)
void handleApiKeysPost() {
  int k = keyIndex(webserver.arg("id"));
  if (k < 0) {
    sendApiResult(false, "bad id");
    return;
  }
  String h = webserver.arg("h");
  h.trim();
  h.toLowerCase();
  if (h.length() == 0) {
    h = KEY_DEFS[k].def;
  } else if (!keyValid(h)) {
    sendApiResult(false, "bad code");
    return;
  }
  keyCode[k] = h;
  keysSave();
  logAdd('W', String("Code de la touche ") + KEY_DEFS[k].name + " : " + (h.length() ? h : String("(aucun)")));
  sendApiResult(true, h);
}
