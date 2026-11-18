// Web page, JSON status and HTTP API.
// The page itself lives in webui.h. Live updates go over the WebSocket on port 81;
// the page falls back to polling /api/status if the WebSocket is not available.

String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

extern char otaState[96];
extern volatile int otaPct;
extern TaskHandle_t otaTask;

String getStatusJSON() {
  float ttt = 0;
  if (heaterState && tubTargetTemp > 0 && tubTemp > 0 && tubTemp < tubTargetTemp) {
    ttt = (tubTargetTemp - tubTemp) * MINUTES_PER_DEGC;
  }
  String j = "{\"type\": \"status\", \"data\" : {";
  j += " \"temp\": \"" + String(tubTemp, 1) + "\"";
  j += ", \"target\": \"" + String(tubTargetTemp, 1) + "\"";
  j += ", \"heater\": " + String(heaterState ? "true" : "false");
  j += ", \"light\": " + String(lightState ? "true" : "false");
  j += ", \"pump1\": " + String(pump1State);
  j += ", \"pump2\": " + String(pump2State);
  j += ", \"p1l\": " + String(pump1Lvl);
  j += ", \"p2l\": " + String(pump2Lvl);
  j += ", \"mode\": \"" + String(modeIdx) + "\"";
  j += ", \"state\": \"" + jsonEscape(state) + "\"";
  j += ", \"time\": \"" + jsonEscape(timeString) + "\"";
  j += ", \"timeToTemp\": " + String((int)ttt);
  j += ", \"queue\": " + String(sendBuffer.itemCount());
  j += ", \"panel\": " + String(panelDetected ? "true" : "false");
  j += ", \"mqtt\": " + String(mqttEnabled ? "true" : "false");
  j += ", \"uptime\": " + String(lastUptime);
  j += ", \"msg\": \"" + jsonEscape(panelMsg) + "\"";
  // true while the display text was seen in the last 60 s (Balboa alternates error codes with the temperature)
  j += ", \"msgActive\": " + String((panelMsgSeen && millis() - panelMsgSeen < 60000UL) ? "true" : "false");
  j += ", \"fw\": \"" FW_VERSION "\"";
  j += ", \"boot\": \"" + jsonEscape(bootReason) + "\"";
  j += ", \"boots\": " + String(bootCount);
  j += ", \"p5\": " + String(pin5Rate);
  j += ", \"ota\": \"" + jsonEscape(String(otaState)) + "\"";
  j += ", \"otaPct\": " + String((int)otaPct);
  j += ", \"sniff\": " + String(sniffOn ? "true" : "false");
  j += " } }";
  return j;
}

void handleRoot() {
  webserver.sendHeader("Cache-Control", "no-cache");
  webserver.send_P(200, "text/html", INDEX_HTML);
}

// The original minimal status page from the upstream project, kept for debugging
void handleRawStatus() {
 webserver.send(200, "text/html", "<html><head><title>Spa Status</title></head><body><script>var wsurl = 'ws://' + window.location.hostname + ':81'; var socket = new WebSocket(wsurl); socket.onmessage = function (event) { console.log(event.data);  var msg = JSON.parse(event.data); document.getElementById('mode').innerHTML = msg.data.mode; document.getElementById('state').innerHTML = msg.data.state; document.getElementById('temp').innerHTML = msg.data.temp; document.getElementById('heater').innerHTML = msg.data.heater; document.getElementById('date').innerHTML = new Date(); }</script>Mode:&nbsp;<span id=\"mode\">"+String(modeIdx)+"</span><br/>State:&nbsp;<span id=\"state\">"+state+"</span><br/>Temp:&nbsp;<span id=\"temp\">" + String(tubTemp,1) + "</span>C<br/>Heating:&nbsp;<span id=\"heater\">"+(heaterState ? "true" : "false")+"</span><br/>Last Update:&nbsp;<span id=\"date\"></span></body></html>");
}

// Raw command passthrough (hex string in the request body) - for debugging only
void handleSend() {

    if (webserver.hasArg("plain")== false){ //Check if body received
          webserver.send(400, "text/plain", "Body not received");
          return;
    }

    String message = webserver.arg("plain");
    webserver.send(200, "text/plain", message);
    logAdd('W', "Pagina /send: " + message);
    sendBuffer.enqueue(message);
}

void sendApiResult(bool ok, const String& msg) {
  if (!ok && msg == "nopanel") logAdd('W', "Comandă refuzată: pinul 5 nu e detectat");
  String j = "{\"ok\": " + String(ok ? "true" : "false") + ", \"msg\": \"" + jsonEscape(msg) + "\"}";
  webserver.send(ok ? 200 : 400, "application/json", j);
}

// /api/status
void handleApiStatus() {
  webserver.send(200, "application/json", getStatusJSON());
}

// /api/cmd?a=up|down|light|jet1|jet2|aux|mode|time
// Each action is one simulated button press on the topside panel.
void handleApiCmd() {
  String a = webserver.arg("a");
  if (otaTask) {
    sendApiResult(false, "busy");
    return;
  }
  if (!panelDetected) {
    sendApiResult(false, "nopanel");
    return;
  }
  if (sendBuffer.itemCount() >= SINGLE_CMD_QUEUE_MAX) {
    logAdd('W', "Pagina: " + a + " refuzat, coada are " + String(sendBuffer.itemCount()) + " comenzi");
    sendApiResult(false, "busy");
    return;
  }
  logAdd('W', "Pagina: buton " + a);
  if (a == "up") {
    sendBuffer.enqueue(COMMAND_UP);
  } else if (a == "down") {
    sendBuffer.enqueue(COMMAND_DOWN);
  } else if (a == "light") {
    sendBuffer.enqueue(COMMAND_LIGHT);
  } else if (a == "jet1") {
    sendBuffer.enqueue(COMMAND_JET1);
  } else if (a == "jet2") {
    sendBuffer.enqueue(COMMAND_JET2);
  } else if (a == "aux") {
    sendBuffer.enqueue(COMMAND_AUX);
  } else if (a == "mode") {
    sendBuffer.enqueue(COMMAND_CHANGE_MODE);
  } else if (a == "time") {
    sendBuffer.enqueue(COMMAND_TIME);
  } else {
    sendApiResult(false, "unknown action");
    return;
  }
  sendApiResult(true, a);
}

// /api/target?t=37.5   (0.5 degree steps, 26-40 C)
void handleApiTarget() {
  if (!webserver.hasArg("t")) {
    sendApiResult(false, "missing t");
    return;
  }
  if (!panelDetected) {
    sendApiResult(false, "nopanel");
    return;
  }
  float t = webserver.arg("t").toFloat();
  t = round(t * 2) / 2.0;
  if (t < 26 || t > 40) {
    sendApiResult(false, "out of range");
    return;
  }
  logAdd('W', "Pagina: temperatura dorită " + String(t, 1) + " (acum " + String(tubTargetTemp, 1) + ")");
  if (setTargetTemperature(t)) {
    sendApiResult(true, "target " + String(t, 1));
  } else {
    sendApiResult(false, "target unknown");
  }
}

// /api/mode?m=0|1|2   (Standard / Economy / Sleep)
void handleApiMode() {
  if (!panelDetected) {
    sendApiResult(false, "nopanel");
    return;
  }
  int m = webserver.arg("m").toInt();
  if (!webserver.hasArg("m") || m < 0 || m > 2) {
    sendApiResult(false, "bad mode");
    return;
  }
  int currentIndex = modeIdx;
  if (currentIndex < 0) {
    sendApiResult(false, "mode unknown");
    return;
  }
  if (currentIndex == m) {
    sendApiResult(true, "no change");
    return;
  }
  logAdd('W', "Pagina: mod " + String(currentIndex) + " -> " + String(m));
  sendBuffer.enqueue(COMMAND_CHANGE_MODE);
  setOption(currentIndex, m, 3, COMMAND_DOWN);
  sendBuffer.enqueue(COMMAND_CHANGE_MODE);
  sendApiResult(true, "mode " + String(m));
}

// POST /api/wifireset - forget the saved WiFi and restart into the setup hotspot
void handleApiWifiReset() {
  sendApiResult(true, "restarting");
  delay(500);
  WiFiManager wm;
  wm.resetSettings();
  restartWith("schimbare rețea WiFi din pagină");
}

// POST /api/restart
void handleApiRestart() {
  sendApiResult(true, "restarting");
  delay(500);
  restartWith("repornire din pagină");
}

// POST /api/queue/clear - drop every command still waiting
void handleApiQueueClear() {
  int n = queueClear();
  logAdd('W', "Pagina: am golit coada (" + String(n) + " comenzi)");
  sendApiResult(true, String(n));
}

// POST /api/log/clear
void handleApiLogClear() {
  logClear();
  logAdd('W', "Jurnal golit");
  sendApiResult(true, "cleared");
}

// POST /api/log/note  (form field t) - a note typed by the user, e.g. "am apăsat Jet pe panou"
void handleApiLogNote() {
  String t = webserver.arg("t");
  t.trim();
  if (t.length() == 0) {
    sendApiResult(false, "empty");
    return;
  }
  logAdd('M', t.substring(0, 100));
  sendApiResult(true, "ok");
}

// POST /api/log/sniff?on=1|0 - log button codes from the physical panel
void handleApiSniff() {
  sniffOn = webserver.arg("on") == "1";
  logAdd('S', sniffOn ? "Captare butoane panou fizic: pornită" : "Captare butoane panou fizic: oprită");
  sendApiResult(true, sniffOn ? "on" : "off");
}

// POST /api/sendhex?h=fb0603450e0006f9xx - send one raw 9-byte button frame (testing codes seen from the panel)
void handleApiSendHex() {
  String h = webserver.arg("h");
  h.trim();
  h.toLowerCase();
  bool ok = h.length() == 18 && h.startsWith("fb06");
  for (size_t i = 0; ok && i < h.length(); i++) ok = isxdigit((unsigned char)h[i]);
  if (!ok) {
    sendApiResult(false, "bad code");
    return;
  }
  if (!panelDetected) {
    sendApiResult(false, "nopanel");
    return;
  }
  if (sendBuffer.itemCount() >= SINGLE_CMD_QUEUE_MAX) {
    sendApiResult(false, "busy");
    return;
  }
  logAdd('W', "Pagina: test cod " + h);
  sendBuffer.enqueue(h);
  sendApiResult(true, h);
}

// ---------------------------------------------------------------------------
// Page settings shared by every phone: header title, function names, which
// functions are shown. Stored on the ESP32 (NVS) so all phones see the same page.
// The cfg blob is JSON written by the page; the firmware only stores it.
// ---------------------------------------------------------------------------
#define UI_TITLE_MAX_BYTES 120
#define UI_CFG_MAX_BYTES 1500

String uiTitle() {
  String t = prefs.getString("uiTitle", "");
  return t.length() > 0 ? t : String("Jacuzzi");
}

// GET /api/config
void handleApiConfigGet() {
  String cfg = prefs.getString("uiCfg", "");
  String j = "{\"title\": \"" + jsonEscape(uiTitle()) + "\", \"cfg\": \"" + jsonEscape(cfg) + "\"}";
  webserver.send(200, "application/json", j);
}

// POST /api/config  (form fields: title, cfg)
void handleApiConfigPost() {
  String title = webserver.arg("title");
  title.trim();
  String cfg = webserver.arg("cfg");
  if (title.length() == 0 || title.length() > UI_TITLE_MAX_BYTES) {
    sendApiResult(false, "bad title");
    return;
  }
  if (cfg.length() > UI_CFG_MAX_BYTES || (cfg.length() > 0 && cfg[0] != '{')) {
    sendApiResult(false, "bad cfg");
    return;
  }
  prefs.putString("uiTitle", title);
  prefs.putString("uiCfg", cfg);
  sendApiResult(true, "saved");
}

// ---------------------------------------------------------------------------
// Home-screen icon: web app manifest (uses the custom title) and PNG icons
// ---------------------------------------------------------------------------
void handleManifest() {
  String t = jsonEscape(uiTitle());
  String j = "{\"name\": \"" + t + "\", \"short_name\": \"" + t + "\", \"start_url\": \"/\", \"scope\": \"/\", "
             "\"display\": \"standalone\", \"background_color\": \"#f3f5f7\", \"theme_color\": \"#0e7490\", "
             "\"icons\": [{\"src\": \"/icon-192.png\", \"sizes\": \"192x192\", \"type\": \"image/png\"}, "
             "{\"src\": \"/icon-512.png\", \"sizes\": \"512x512\", \"type\": \"image/png\"}]}";
  webserver.send(200, "application/manifest+json", j);
}

void sendIcon(const uint8_t* data, size_t len) {
  webserver.sendHeader("Cache-Control", "max-age=604800");
  webserver.send_P(200, "image/png", (PGM_P)data, len);
}
void handleIcon180() { sendIcon(ICON_180_PNG, sizeof(ICON_180_PNG)); }
void handleIcon192() { sendIcon(ICON_192_PNG, sizeof(ICON_192_PNG)); }
void handleIcon512() { sendIcon(ICON_512_PNG, sizeof(ICON_512_PNG)); }

// ---------------------------------------------------------------------------
// Water temperature history: one sample every 5 minutes, last 24 hours, in RAM
// (lost on restart - writing it to flash every 5 minutes would wear the flash).
// ---------------------------------------------------------------------------
#define HIST_N 288
#define HIST_INTERVAL_MS 300000UL
#define HIST_GAP INT16_MIN

int16_t histTemp[HIST_N];   // degrees C x10, HIST_GAP when no reading
uint8_t histHeat[HIST_N];   // 1 when the heater was on
uint16_t histCount = 0;
uint16_t histHead = 0;      // next slot to write
unsigned long histLast = 0;

void historyTick() {
  unsigned long now = millis();
  if (histCount == 0) {
    if (tubTemp <= 0) return;  // start with the first real reading
  } else if (now - histLast < HIST_INTERVAL_MS) {
    return;
  }
  histLast = now;
  histTemp[histHead] = tubTemp > 0 ? (int16_t)lround(tubTemp * 10) : HIST_GAP;
  histHeat[histHead] = heaterState ? 1 : 0;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

// GET /api/history -> {"interval": 300, "age": <s since newest>, "t": [oldest..newest], "h": "0101.."}
void handleApiHistory() {
  String t;
  String h;
  t.reserve(histCount * 5 + 8);
  h.reserve(histCount + 2);
  uint16_t start = (histHead + HIST_N - histCount) % HIST_N;
  for (uint16_t i = 0; i < histCount; i++) {
    uint16_t k = (start + i) % HIST_N;
    if (i) t += ',';
    if (histTemp[k] == HIST_GAP) {
      t += "null";
    } else {
      t += String(histTemp[k] / 10.0, 1);
    }
    h += histHeat[k] ? '1' : '0';
  }
  unsigned long age = histCount ? (millis() - histLast) / 1000 : 0;
  String j = "{\"interval\": " + String(HIST_INTERVAL_MS / 1000) + ", \"age\": " + String(age) +
             ", \"t\": [" + t + "], \"h\": \"" + h + "\"}";
  webserver.send(200, "application/json", j);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {

  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

        // send message to client
        String json = getStatusJSON();
        Serial.println(json);
        webSocket.sendTXT(num, json);
      }
      break;
    case WStype_TEXT:
    case WStype_BIN:
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }

}
