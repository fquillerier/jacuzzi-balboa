// Bus spy ("espion"): records the raw frames seen on the RS485 bus.
//
//  mode 1 "simple"  : a frame is recorded only when it differs from the previous frame of the same kind
//  mode 2 "avancé"  : every frame is recorded
//
// The frames go into a small ring buffer in RAM (allocated only while the spy is on, never written to flash).
// When it is full the oldest frames are overwritten. The web page polls /api/spy, keeps the whole history
// on the phone and saves it to a file: the ESP32 itself only holds the last few seconds / minutes.
//
// Our own slot (pin 5 low) is recorded from handleMessage(). The other panels' slots are read here,
// split into frames by their first byte (FA = 23 bytes, AE = 16, FB = 9).

void notePanelButton(const uint8_t* p, const char* where);  // defined in sensor.cpp

#define SPY_FRAME_MAX 23
#define SPY_SIG_N 16          // how many "kinds" of frame the simple mode remembers

struct SpyFrame {
  uint32_t seq;
  uint32_t ms;
  uint8_t flags;              // bit0: 1 = our slot (pin 5 low), 0 = another panel's slot
  uint8_t len;
  uint8_t d[SPY_FRAME_MAX];
};

uint8_t spyMode = 0;          // 0 off, 1 simple, 2 advanced
SpyFrame* spyBuf = nullptr;
uint16_t spyCap = 0;
uint32_t spySeq = 0;          // seq of the next frame to record (= total recorded since start)
uint32_t spySeen = 0;         // frames seen (recorded or not)
uint32_t spySeenLastSec = 0;
uint16_t spyRate = 0;         // frames seen per second
unsigned long spyRateAt = 0;
unsigned long spyStartMs = 0;

// last frame of each kind, for the simple mode
struct SpySig {
  uint8_t used;
  uint8_t ours;
  uint8_t len;
  uint8_t d[SPY_FRAME_MAX];
};
SpySig spySigs[SPY_SIG_N];
uint8_t spySigNext = 0;

// framer for the other panels' slots
uint8_t spyRx[SPY_FRAME_MAX];
uint8_t spyRxLen = 0;
uint8_t spyRxWant = 0;
uint32_t spyEdgesSeen = 0;

uint8_t spyFrameLen(uint8_t first) {
  switch (first) {
    case 0xFA: return 23;
    case 0xAE: return 16;
    case 0xFB: return 9;
    default: return 0;
  }
}

// how many leading bytes identify the "kind" of a frame (simple mode compares frames of the same kind)
uint8_t spyKindLen(const uint8_t* d, uint8_t len) {
  if (d[0] == 0xAE) return len >= 3 ? 3 : len;   // ae 0d 01 / 02 / 03 are different messages
  if (d[0] == 0xFB) return len >= 5 ? 5 : len;   // fb 06 + panel address
  return len >= 2 ? 2 : len;                     // fa 14
}

bool spyIsNew(const uint8_t* d, uint8_t len, uint8_t ours) {
  uint8_t k = spyKindLen(d, len);
  for (int i = 0; i < SPY_SIG_N; i++) {
    SpySig& s = spySigs[i];
    if (!s.used || s.ours != ours || s.len != len || memcmp(s.d, d, k) != 0) continue;
    if (memcmp(s.d, d, len) == 0) return false;   // same as the previous frame of this kind
    memcpy(s.d, d, len);
    return true;
  }
  SpySig& s = spySigs[spySigNext];                // first frame of a new kind
  spySigNext = (spySigNext + 1) % SPY_SIG_N;
  s.used = 1;
  s.ours = ours;
  s.len = len;
  memcpy(s.d, d, len);
  return true;
}

void spyTickRate() {
  unsigned long now = millis();
  if (now - spyRateAt >= 1000) {
    spyRate = (uint16_t)min<uint32_t>(spySeen - spySeenLastSec, 65535);
    spySeenLastSec = spySeen;
    spyRateAt = now;
  }
}

// Keep this cheap: it runs right in the bus loop.
void spyRecord(const uint8_t* d, uint8_t len, uint8_t ours) {
  if (!spyMode || !spyBuf || len == 0) return;
  if (len > SPY_FRAME_MAX) len = SPY_FRAME_MAX;
  spySeen++;
  if (spyMode == 1 && !spyIsNew(d, len, ours)) return;
  SpyFrame& f = spyBuf[spySeq % spyCap];
  f.seq = spySeq++;
  f.ms = millis();
  f.flags = ours ? 1 : 0;
  f.len = len;
  memcpy(f.d, d, len);
}

// Read the bus while another panel is being served (pin 5 high). Also feeds the button capture.
void spyRead() {
  // the RX buffer is cleared at every falling edge of pin 5: a frame in progress is lost
  uint32_t e = pin5Edges;
  if (e != spyEdgesSeen) {
    spyEdgesSeen = e;
    spyRxLen = 0;
  }
  int guard = 64;
  while (tub.available() > 0 && guard-- > 0) {
    uint8_t b = tub.read();
    if (spyRxLen == 0) {
      spyRxWant = spyFrameLen(b);
      if (!spyRxWant) continue;                   // not a frame start: skip
    }
    spyRx[spyRxLen++] = b;
    if (spyRxLen == 2) {
      // second byte must match the frame type (fa 14 / ae 0d / fb 06), else we were out of step
      uint8_t ok2 = spyRx[0] == 0xFA ? 0x14 : (spyRx[0] == 0xAE ? 0x0D : 0x06);
      if (b != ok2) {
        spyRxWant = spyFrameLen(b);
        if (spyRxWant) {
          spyRx[0] = b;
          spyRxLen = 1;
        } else {
          spyRxLen = 0;
        }
        continue;
      }
    }
    if (spyRxLen == spyRxWant) {
      spyRecord(spyRx, spyRxLen, 0);
      if (sniffOn && spyRx[0] == 0xFB && spyRx[1] == 0x06) notePanelButton(spyRx, "bus");
      spyRxLen = 0;
    }
  }
}

bool spySetMode(uint8_t m) {
  if (m > 2) return false;
  if (m == 0) {
    if (spyBuf) free(spyBuf);
    spyBuf = nullptr;
    spyCap = 0;
    spyMode = 0;
    logAdd('S', "Espion arrêté");
    return true;
  }
  if (!spyBuf) {
    // ~54 KB, or less if the memory is short; keep at least 40 KB free for Wi-Fi and the web server
    const uint16_t caps[] = {1500, 1000, 600, 300};
    for (uint16_t c : caps) {
      size_t need = (size_t)c * sizeof(SpyFrame);
      if (ESP.getFreeHeap() < need + 40000) continue;
      spyBuf = (SpyFrame*)malloc(need);
      if (spyBuf) {
        spyCap = c;
        break;
      }
    }
    if (!spyBuf) {
      logAdd('E', "Espion : pas assez de mémoire libre");
      return false;
    }
    spySeq = 0;
    spySeen = 0;
    spySeenLastSec = 0;
    spyRateAt = millis();
    spyStartMs = millis();
    spyRxLen = 0;
    memset(spySigs, 0, sizeof(spySigs));
  } else if (m == 1 && spyMode != 1) {
    memset(spySigs, 0, sizeof(spySigs));         // simple mode starts from a clean slate
  }
  spyMode = m;
  logAdd('S', String("Espion ") + (m == 1 ? "simple" : "avancé") + " activé (" + String(spyCap) + " trames en mémoire)");
  return true;
}

void spyClear() {
  if (!spyBuf) return;
  spySeq = 0;
  spySeen = 0;
  spySeenLastSec = 0;
  memset(spySigs, 0, sizeof(spySigs));
}

// POST /api/spy/mode?m=0|1|2
void handleApiSpyMode() {
  long m = webserver.arg("m").toInt();
  bool ok = spySetMode((uint8_t)m);
  sendApiResult(ok, ok ? String(spyCap) : String("no memory"));
}

// POST /api/spy/clear
void handleApiSpyClear() {
  spyClear();
  sendApiResult(true, "ok");
}

// GET /api/spy?since=<seq>&max=<n>
// -> {"mode":1,"cap":1500,"seq":<next>,"first":<oldest kept>,"rate":<frames/s>,"seen":<total seen>,
//     "start":<ms when started>,"now":<ms>,"heap":<free bytes>,"frames":[[seq,ms,ours,"hex"],...]}
void handleApiSpy() {
  uint32_t first = spySeq > spyCap ? spySeq - spyCap : 0;
  uint32_t since = webserver.hasArg("since") ? (uint32_t)webserver.arg("since").toInt() : first;
  if (since < first) since = first;
  if (since > spySeq) since = spySeq;
  long mx = webserver.hasArg("max") ? webserver.arg("max").toInt() : 400;
  if (mx < 1) mx = 1;
  if (mx > 600) mx = 600;
  uint32_t end = spySeq;
  if (end - since > (uint32_t)mx) end = since + mx;

  String head = "{\"mode\":" + String(spyMode) + ",\"cap\":" + String(spyCap) + ",\"seq\":" + String(end) +
                ",\"last\":" + String(spySeq) + ",\"first\":" + String(first) + ",\"rate\":" + String(spyRate) +
                ",\"seen\":" + String(spySeen) + ",\"start\":" + String(spyStartMs) + ",\"now\":" + String(millis()) +
                ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"frames\":[";
  webserver.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webserver.send(200, "application/json", head);

  static const char hx[] = "0123456789abcdef";
  String chunk;
  chunk.reserve(2200);
  for (uint32_t s = since; spyBuf && s < end; s++) {
    const SpyFrame& f = spyBuf[s % spyCap];
    if (s != since) chunk += ',';
    chunk += '[';
    chunk += String(f.seq);
    chunk += ',';
    chunk += String(f.ms);
    chunk += ',';
    chunk += String(f.flags & 1);
    chunk += ",\"";
    for (uint8_t i = 0; i < f.len; i++) {
      chunk += hx[f.d[i] >> 4];
      chunk += hx[f.d[i] & 0xF];
    }
    chunk += "\"]";
    if (chunk.length() > 2000) {
      webserver.sendContent(chunk);
      chunk = "";
    }
  }
  chunk += "]}";
  webserver.sendContent(chunk);
  webserver.sendContent("");
}
