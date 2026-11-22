// Debug log, shown on the web page under "Journal (dépannage)".
// Line types:
//   R = received from the tub controller (display / status changes)
//   T = command sent by us (and its result)
//   P = button code seen from the physical panel
//   W = action requested from the web page
//   S = system (boot, WiFi, pin 5, watchdog)
//   E = error
//   M = note typed by the user in the log page
//
// Surviving restarts:
//  - The whole log lives in RAM that is not cleared on restart (__NOINIT_ATTR), so after a watchdog
//    restart, a crash, an update or a restart from the page, the lines from before are still there.
//  - That RAM is lost when the power is cut, so the newest lines (up to LOG_FLASH_BYTES) are also copied
//    to flash: every 10 minutes when something changed, at most every 5 minutes after an error, and
//    right before a planned restart. After a power cut the log starts from that copy.
// Each line keeps the restart number and, once the clock is known (internet time), the date and time.

#include <time.h>
#include <sys/time.h>

#define LOG_N 200
#define LOG_LEN 120
#define LOG_MAGIC 0x4A4C4F47UL
#define LOG_FLASH_BYTES 3900  // NVS strings are limited to 4000 bytes
#define LOG_FLASH_EVERY_MS 600000UL
#define LOG_FLASH_ERR_MS 300000UL
#define CLOCK_VALID 1700000000UL  // any epoch before this means "clock not set yet"

struct LogLine {
  uint32_t seq;
  uint32_t ms;     // millis() in its own boot
  uint32_t epoch;  // unix time, 0 if the clock was not known yet
  uint16_t boot;   // restart number the line belongs to
  char type;
  char text[LOG_LEN];
};

__NOINIT_ATTR LogLine logBuf[LOG_N];
__NOINIT_ATTR uint32_t logSeq;    // sequence number of the newest line (0 = empty)
__NOINIT_ATTR uint32_t logMagic;  // LOG_MAGIC when the RAM above holds a valid log

uint16_t logBoot = 0;
uint32_t logFlashSeq = 0;         // newest line already copied to flash
unsigned long logFlashAt = 0;
bool logFlashUrgent = false;
bool clockFixed = false;

uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return (uint32_t)t >= CLOCK_VALID ? (uint32_t)t : 0;
}

void logStore(char type, const char* text, uint32_t epoch, uint16_t boot, uint32_t ms) {
  logSeq++;
  LogLine& l = logBuf[logSeq % LOG_N];
  l.seq = logSeq;
  l.ms = ms;
  l.epoch = epoch;
  l.boot = boot;
  l.type = type;
  strlcpy(l.text, text, LOG_LEN);
}

void logAdd(char type, const String& msg) {
  logStore(type, msg.c_str(), nowEpoch(), logBoot, millis());
  if (type == 'E') logFlashUrgent = true;
  Serial.printf("[%c] %s\n", type, msg.c_str());
  if (WiFi.status() == WL_CONNECTED) telnetSend(String(type) + " " + msg);
}

void logClear() {
  for (int i = 0; i < LOG_N; i++) logBuf[i].seq = 0;
}

// Flash copy: lines "epoch|boot|ms|type|text\n", oldest first, newest that fit in LOG_FLASH_BYTES
void logSaveFlash() {
  if (logSeq == logFlashSeq) return;
  String out;
  out.reserve(LOG_FLASH_BYTES + 200);
  uint32_t first = logSeq > LOG_N ? logSeq - LOG_N + 1 : 1;
  // walk back from the newest line until the budget is used
  uint32_t start = logSeq + 1;
  size_t total = 0;
  for (uint32_t s = logSeq; s >= first && s > 0; s--) {
    LogLine& l = logBuf[s % LOG_N];
    if (l.seq != s) break;
    size_t len = strlen(l.text) + 32;
    if (total + len > LOG_FLASH_BYTES) break;
    total += len;
    start = s;
  }
  for (uint32_t s = start; s <= logSeq; s++) {
    LogLine& l = logBuf[s % LOG_N];
    if (l.seq != s) continue;
    out += String(l.epoch) + "|" + String(l.boot) + "|" + String(l.ms) + "|" + String(l.type) + "|" + l.text + "\n";
  }
  prefs.putString("logbak", out);
  logFlashSeq = logSeq;
  logFlashAt = millis();
  logFlashUrgent = false;
}

void logLoadFlash() {
  String in = prefs.getString("logbak", "");
  int pos = 0;
  while (pos < (int)in.length()) {
    int nl = in.indexOf('\n', pos);
    if (nl < 0) break;
    String line = in.substring(pos, nl);
    pos = nl + 1;
    int a = line.indexOf('|'), b = line.indexOf('|', a + 1), c = line.indexOf('|', b + 1), d = line.indexOf('|', c + 1);
    if (a < 0 || b < 0 || c < 0 || d < 0 || d != c + 2) continue;
    logStore(line[c + 1], line.substring(d + 1).c_str(), strtoul(line.substring(0, a).c_str(), NULL, 10),
             (uint16_t)line.substring(a + 1, b).toInt(), strtoul(line.substring(b + 1, c).c_str(), NULL, 10));
  }
}

// Called once at boot, before the first logAdd()
void logBegin(uint16_t boot, bool powerLoss) {
  logBoot = boot;
  bool ramOk = !powerLoss && logMagic == LOG_MAGIC && logSeq < 100000000UL;
  if (ramOk) {
    for (int i = 0; i < LOG_N; i++) logBuf[i].text[LOG_LEN - 1] = 0;  // never trust an unterminated string
  } else {
    logSeq = 0;
    logClear();
    logMagic = LOG_MAGIC;
    logLoadFlash();
  }
  logFlashSeq = logSeq;
  logFlashAt = millis();
}

// Internet time (falls back to the phone's clock, sent by the page)
void clockBegin() {
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.google.com");
}

// Give lines written before the clock was known their real time
void clockFixLines() {
  uint32_t ep = nowEpoch();
  if (ep == 0 || clockFixed) return;
  clockFixed = true;
  uint32_t nowMs = millis();
  for (int i = 0; i < LOG_N; i++) {
    LogLine& l = logBuf[i];
    if (l.seq && l.boot == logBoot && l.epoch == 0 && l.ms <= nowMs) l.epoch = ep - (nowMs - l.ms) / 1000;
  }
}

void logTick() {
  clockFixLines();
  unsigned long now = millis();
  if (logSeq == logFlashSeq) return;
  if (now - logFlashAt >= LOG_FLASH_EVERY_MS || (logFlashUrgent && now - logFlashAt >= LOG_FLASH_ERR_MS)) {
    logSaveFlash();
  }
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

// GET /api/log?since=<seq>
//  -> {"now": <ms>, "epoch": <unix or 0>, "boot": <n>, "seq": <newest>, "lines": [[seq, ms, epoch, boot, "T", "text"], ...]}
void handleApiLog() {
  uint32_t since = webserver.hasArg("since") ? strtoul(webserver.arg("since").c_str(), NULL, 10) : 0;
  if (since > logSeq) since = 0;
  uint32_t first = logSeq > LOG_N ? logSeq - LOG_N + 1 : 1;
  if (since + 1 > first) first = since + 1;
  String j;
  j.reserve(200 + (logSeq - first + 1) * 70);
  j = "{\"now\": " + String(millis()) + ", \"epoch\": " + String(nowEpoch()) + ", \"boot\": " + String(logBoot) +
      ", \"seq\": " + String(logSeq) + ", \"lines\": [";
  bool comma = false;
  for (uint32_t s = first; s <= logSeq && s > 0; s++) {
    LogLine& l = logBuf[s % LOG_N];
    if (l.seq != s) continue;
    if (comma) j += ',';
    comma = true;
    j += "[" + String(l.seq) + "," + String(l.ms) + "," + String(l.epoch) + "," + String(l.boot) + ",\"" +
         String(l.type) + "\",\"" + logEscape(l.text) + "\"]";
  }
  j += "]}";
  webserver.send(200, "application/json", j);
}

// POST /api/time?t=<unix> - the page sends the phone's clock when the module has no internet time
void handleApiTime() {
  uint32_t t = strtoul(webserver.arg("t").c_str(), NULL, 10);
  if (nowEpoch() == 0 && t >= CLOCK_VALID) {
    struct timeval tv = {(time_t)t, 0};
    settimeofday(&tv, nullptr);
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
    logAdd('S', "Heure réglée depuis le téléphone");
  }
  webserver.send(200, "application/json", "{\"ok\": true}");
}
