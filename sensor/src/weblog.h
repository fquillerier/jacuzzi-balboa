// Debug log kept in RAM (ring buffer), shown on the web page under Settings -> Jurnal.
// Line types:
//   R = received from the tub controller (display / status changes)
//   T = command sent by us (and its result)
//   P = button code seen from the physical panel
//   W = action requested from the web page
//   S = system (boot, WiFi, pin 5, watchdog)
//   E = error
//   M = note typed by the user in the log page
// Lost on restart, except the restart reason which is kept in flash.

#define LOG_N 200
#define LOG_LEN 120

struct LogLine {
  uint32_t seq;
  uint32_t ms;
  char type;
  char text[LOG_LEN];
};

LogLine logBuf[LOG_N];
uint32_t logSeq = 0;  // sequence number of the newest line (0 = empty)

void logAdd(char type, const String& msg) {
  logSeq++;
  LogLine& l = logBuf[logSeq % LOG_N];
  l.seq = logSeq;
  l.ms = millis();
  l.type = type;
  strlcpy(l.text, msg.c_str(), LOG_LEN);
  Serial.printf("[%c] %s\n", type, l.text);
  if (WiFi.status() == WL_CONNECTED) telnetSend(String(type) + " " + msg);
}

void logClear() {
  for (int i = 0; i < LOG_N; i++) logBuf[i].seq = 0;
}

String logEscape(const char* s) {
  String out;
  for (; *s; s++) {
    char c = *s;
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

// GET /api/log?since=<seq>  ->  {"now": <ms>, "seq": <newest>, "lines": [[seq, ms, "T", "text"], ...]}
void handleApiLog() {
  uint32_t since = webserver.hasArg("since") ? strtoul(webserver.arg("since").c_str(), NULL, 10) : 0;
  if (since > logSeq) since = 0;  // the module restarted since the page last asked
  uint32_t first = logSeq > LOG_N ? logSeq - LOG_N + 1 : 1;
  if (since + 1 > first) first = since + 1;
  String j;
  j.reserve(200 + (logSeq - first + 1) * 60);
  j = "{\"now\": " + String(millis()) + ", \"seq\": " + String(logSeq) + ", \"lines\": [";
  bool comma = false;
  for (uint32_t s = first; s <= logSeq && s > 0; s++) {
    LogLine& l = logBuf[s % LOG_N];
    if (l.seq != s) continue;  // cleared
    if (comma) j += ',';
    comma = true;
    j += "[" + String(l.seq) + "," + String(l.ms) + ",\"" + String(l.type) + "\",\"" + logEscape(l.text) + "\"]";
  }
  j += "]}";
  webserver.send(200, "application/json", j);
}
