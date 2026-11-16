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
  j += ", \"mode\": \"" + String((int)tubMode.getCurrentState()) + "\"";
  j += ", \"state\": \"" + jsonEscape(state) + "\"";
  j += ", \"time\": \"" + jsonEscape(timeString) + "\"";
  j += ", \"timeToTemp\": " + String((int)ttt);
  j += ", \"queue\": " + String(sendBuffer.itemCount());
  j += ", \"panel\": " + String(panelDetected ? "true" : "false");
  j += ", \"mqtt\": " + String(mqttEnabled ? "true" : "false");
  j += ", \"uptime\": " + String(lastUptime);
  j += ", \"fw\": \"" FW_VERSION "\"";
  j += " } }";
  return j;
}

void handleRoot() {
  webserver.send_P(200, "text/html", INDEX_HTML);
}

// The original minimal status page from the upstream project, kept for debugging
void handleRawStatus() {
 webserver.send(200, "text/html", "<html><head><title>Spa Status</title></head><body><script>var wsurl = 'ws://' + window.location.hostname + ':81'; var socket = new WebSocket(wsurl); socket.onmessage = function (event) { console.log(event.data);  var msg = JSON.parse(event.data); document.getElementById('mode').innerHTML = msg.data.mode; document.getElementById('state').innerHTML = msg.data.state; document.getElementById('temp').innerHTML = msg.data.temp; document.getElementById('heater').innerHTML = msg.data.heater; document.getElementById('date').innerHTML = new Date(); }</script>Mode:&nbsp;<span id=\"mode\">"+(String)tubMode.getCurrentState()+"</span><br/>State:&nbsp;<span id=\"state\">"+state+"</span><br/>Temp:&nbsp;<span id=\"temp\">" + String(tubTemp,1) + "</span>C<br/>Heating:&nbsp;<span id=\"heater\">"+(heaterState ? "true" : "false")+"</span><br/>Last Update:&nbsp;<span id=\"date\"></span></body></html>");
}

// Raw command passthrough (hex string in the request body) - for debugging only
void handleSend() {

    if (webserver.hasArg("plain")== false){ //Check if body received
          webserver.send(400, "text/plain", "Body not received");
          return;
    }

    String message = webserver.arg("plain");
    webserver.send(200, "text/plain", message);
    Serial.printf("Send data: %s\n", message.c_str());
    sendBuffer.enqueue(message);
}

void sendApiResult(bool ok, const String& msg) {
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
  if (sendBuffer.isFull()) {
    sendApiResult(false, "busy");
    return;
  }
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
  float t = webserver.arg("t").toFloat();
  t = round(t * 2) / 2.0;
  if (t < 26 || t > 40) {
    sendApiResult(false, "out of range");
    return;
  }
  if (setTargetTemperature(t)) {
    sendApiResult(true, "target " + String(t, 1));
  } else {
    sendApiResult(false, "target unknown");
  }
}

// /api/mode?m=0|1|2   (Standard / Economy / Sleep)
void handleApiMode() {
  int m = webserver.arg("m").toInt();
  if (!webserver.hasArg("m") || m < 0 || m > 2) {
    sendApiResult(false, "bad mode");
    return;
  }
  int currentIndex = tubMode.getCurrentState();
  if (currentIndex == m) {
    sendApiResult(true, "no change");
    return;
  }
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
  ESP.restart();
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
