// Online update from the GitHub releases of this project.
// The page (on the phone) asks the GitHub API for the newest release, shows what changed, and on
// "Installer" sends the ESP32 the download link and the SHA-256 of the file. The ESP32 downloads the
// file itself, checks the SHA-256 and only then switches to the new firmware. If anything fails it
// keeps running the current version (the new one is written to the other half of the flash).
//
// The download link must point to this repository's releases, so the page can't be used to install
// some other firmware. The ESP32 doesn't check GitHub's certificate (no certificate store on board),
// which is why the SHA-256 check matters: the phone gets it from the GitHub API over a verified link.

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#define OTA_URL_PREFIX "https://github.com/fquillerier/jacuzzi-balboa/releases/download/"

volatile int otaPct = -1;        // -1 idle, 0..100 while downloading
char otaState[96] = "";          // "", "download", "error: ...", "done" (char array: written by the update task, read by the page)
String otaUrl, otaSha, otaVer;
TaskHandle_t otaTask = NULL;

// "1.10.2" > "1.9.0"
int versionCmp(const String& a, const String& b) {
  int ai = 0, bi = 0;
  for (int part = 0; part < 4; part++) {
    long x = 0, y = 0;
    while (ai < (int)a.length() && isdigit((unsigned char)a[ai])) x = x * 10 + (a[ai++] - '0');
    while (bi < (int)b.length() && isdigit((unsigned char)b[bi])) y = y * 10 + (b[bi++] - '0');
    if (x != y) return x < y ? -1 : 1;
    if (ai < (int)a.length()) ai++;
    if (bi < (int)b.length()) bi++;
  }
  return 0;
}

void otaFail(const String& why) {
  strlcpy(otaState, ("error: " + why).c_str(), sizeof(otaState));
  otaPct = -1;
  logAdd('E', "Mise à jour en ligne échouée : " + why);
  attachPanelInterrupt();
}

void otaRun(void* arg) {
  logAdd('S', "Mise à jour en ligne : téléchargement " + otaVer);
  detachInterrupt(digitalPinToInterrupt(PIN_5_PIN_DEF));  // as for the local update: no pin 5 work while writing flash

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  bool ok = false;
  do {
    if (!http.begin(client, otaUrl)) {
      otaFail("impossible d'ouvrir l'adresse");
      break;
    }
    int code = http.GET();
    if (code != 200) {
      otaFail("GitHub a répondu " + String(code));
      break;
    }
    int len = http.getSize();
    if (len <= 0) {
      otaFail("taille inconnue");
      break;
    }
    if (!Update.begin(len, U_FLASH)) {
      otaFail("pas de place pour " + String(len) + " octets");
      break;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[2048];
    int done = 0;
    unsigned long lastData = millis();
    bool streamOk = true;
    while (done < len) {
      size_t avail = stream->available();
      if (avail) {
        int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (n <= 0) continue;
        mbedtls_sha256_update(&sha, buf, n);
        if (Update.write(buf, n) != (size_t)n) {
          streamOk = false;
          otaFail("écriture en mémoire flash échouée");
          break;
        }
        done += n;
        otaPct = (int)((int64_t)done * 100 / len);
        lastData = millis();
      } else {
        if (!http.connected() || millis() - lastData > 20000) {
          streamOk = false;
          otaFail("téléchargement interrompu à " + String(otaPct) + "%");
          break;
        }
        delay(5);
      }
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!streamOk) {
      Update.abort();
      break;
    }
    String hex = "";
    for (int i = 0; i < 32; i++) {
      if (digest[i] < 0x10) hex += '0';
      hex += String(digest[i], HEX);
    }
    if (hex != otaSha) {
      Update.abort();
      otaFail("fichier corrompu (SHA-256 différent)");
      break;
    }
    if (!Update.end(true)) {
      otaFail("vérification échouée (" + String(Update.errorString()) + ")");
      break;
    }
    ok = true;
  } while (false);
  http.end();

  if (ok) {
    strlcpy(otaState, "done", sizeof(otaState));
    otaPct = 100;
    delay(1500);  // let the page see "done"
    restartWith("update online la " + otaVer);
  }
  otaTask = NULL;
  vTaskDelete(NULL);
}

// POST /api/ota?url=...&sha=<64 hex>&ver=1.6.0
void handleApiOta() {
  String url = webserver.arg("url");
  String sha = webserver.arg("sha");
  String ver = webserver.arg("ver");
  sha.toLowerCase();
  if (ver.startsWith("v")) ver = ver.substring(1);
  if (otaTask) {
    sendApiResult(false, "already running");
    return;
  }
  if (!url.startsWith(OTA_URL_PREFIX) || url.indexOf("..") >= 0) {
    sendApiResult(false, "bad url");
    return;
  }
  bool shaOk = sha.length() == 64;
  for (size_t i = 0; shaOk && i < sha.length(); i++) shaOk = isxdigit((unsigned char)sha[i]);
  if (!shaOk) {
    sendApiResult(false, "bad sha");
    return;
  }
  if (versionCmp(ver, FW_VERSION) <= 0) {
    sendApiResult(false, "not newer");
    return;
  }
  if (!sendBuffer.isEmpty()) {
    sendApiResult(false, "busy");
    return;
  }
  otaUrl = url;
  otaSha = sha;
  otaVer = ver;
  strlcpy(otaState, "download", sizeof(otaState));
  otaPct = 0;
  logAdd('W', "Page : mise à jour en ligne vers " + ver);
  if (xTaskCreatePinnedToCore(otaRun, "otaOnline", 16384, NULL, 1, &otaTask, 0) != pdPASS) {
    otaTask = NULL;
    strlcpy(otaState, "error: démarrage impossible", sizeof(otaState));
    otaPct = -1;
    sendApiResult(false, "no task");
    return;
  }
  sendApiResult(true, "started");
}
