#ifdef ESP32
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiAP.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <driver/uart.h>
#else
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiAP.h>
#include <ESP8266mDNS.h>
#include <SoftwareSerial.h>  // - https://github.com/plerup/espsoftwareserial
#endif
#include <ArduinoHA.h>
#include <ArduinoQueue.h>
#include <WebSocketsServer.h>
#include <WiFiUdp.h>
#include <WebOTA.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager
#include <Preferences.h>

// ************************************************************************************************
// Start of config
// ************************************************************************************************

#define WDT_TIMEOUT 30

// Generic ESP32 DevKit boards don't define the on-board LED; it is GPIO2 on most of them
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#define FW_VERSION "1.6.0"

// Name of the setup hotspot shown on the phone on first start (or when the saved WiFi is not reachable)
#define SETUP_AP_NAME "Jacuzzi-Setup"
// Seconds the setup hotspot stays up before the ESP restarts and tries the saved WiFi again
#define SETUP_PORTAL_TIMEOUT 300
// mDNS name: the page is reachable at http://jacuzzi.local
#define MDNS_NAME "jacuzzi"

// MQTT / Home Assistant is optional. Leave the broker empty in the setup page to disable it.
Preferences prefs;
char mqttHost[64] = "";
char mqttPort[6] = "1883";
char mqttUser[33] = "";
char mqttPass[65] = "";
bool mqttEnabled = false;

byte mac[] = {0x00, 0x10, 0xFA, 0x6E, 0x38, 0x4A};  // Leave this value, unless you own multiple hot tubs

// Perform measurements or read nameplate values on your tub to define the power [kW]
// for each device in order to calculate tub power usage
const float POWER_HEATER = 2.8;
const float POWER_PUMP_CIRCULATION = 0.3;
const float POWER_PUMP1_LOW = 0.31;
const float POWER_PUMP1_HIGH = 1.3;
const float POWER_PUMP2_LOW = 0.3;
const float POWER_PUMP2_HIGH = 0.6;

// Tweak for your tub - would be nice to auto-learn in the future to allow for outside temp etc
const int MINUTES_PER_DEGC = 45;

#define DELAY_TIME_DEFAULT 20
int delayTime = DELAY_TIME_DEFAULT;


#ifdef RSC3
#define tub Serial1
#define tubUART UART_NUM_1
#define RX_PIN 3
#define TX_PIN 10
#define RTS_PIN_DEF 5  // RS485 direction control, RequestToSend TX or RX, required for MAX485 board.
#define PIN_5_PIN_DEF 6

#include <Adafruit_NeoPixel.h>
//Adafruit_NeoPixel pixels(1, 4, NEO_GRB + NEO_KHZ800);

#elif ESP32
#define tub Serial2
#define tubUART UART_NUM_2
#define RX_PIN 19
#define TX_PIN 23
#define RTS_PIN_DEF 22  // RS485 direction control, RequestToSend TX or RX, required for MAX485 board.
#define PIN_5_PIN_DEF 18
#else
SoftwareSerial tub;
#define RX_PIN D6
#define TX_PIN D7
#define PIN_5_PIN D4
#define RTS_PIN D1  // RS485 direction control, RequestToSend TX or RX, required for MAX485 board.
#endif

// Uncomment if you have dual-speed pump
// #define PUMP1_DUAL_SPEED
// #define PUMP2_DUAL_SPEED
// #define AUX_DUAL_SPEED

// #define USE_FAHRENHEIT

// ************************************************************************************************
// End of config
// ************************************************************************************************

#include "constants.h"

WiFiClient clients[1];

HADevice device(mac, sizeof(mac));
HAMqtt mqtt(clients[0], device, 30);
HASensorNumber temp("temp", HANumber::PrecisionP1);
HASensorNumber targetTemp("targetTemp", HANumber::PrecisionP1);
HASensorNumber timeToTemp("timeToTemp");
HASensor currentState("status");
HASensor haTime("time");
HASensor rawData("raw");
HASensor rawData2("raw2");
HASensor rawData3("raw3");
HASensor fbData("fb");
HASelect tubMode("mode");
HASensorNumber uptime("uptime");
HASelect pump1("pump1");
HASelect pump2("pump2");
HASelect aux("aux");
HABinarySensor heater("heater");
HASwitch light("light");
HASensorNumber tubpower("tubpower", HANumber::PrecisionP1);
HASensorNumber commandQueueSize("commandQueueSize",  HANumber::PrecisionP0);
HASensorNumber timeSinceMsgStartSensor("timeSinceMsgStart",  HANumber::PrecisionP0);

HAButton btnUp("up");
HAButton btnDown("down");
HAButton btnMode("btnMode");
HAButton btnTime("btnTime");

// Not really HVAC device, but only way to get controls to set
HAHVAC hvac("temp", HAHVAC::TargetTemperatureFeature);

#define MAX_SRV_CLIENTS 2
WiFiServer server(23);
WiFiClient serverClients[MAX_SRV_CLIENTS];

WebSocketsServer webSocket = WebSocketsServer(81);
#ifdef ESP32
WebServer webserver(80);
#else
ESP8266WebServer webserver(80);
#endif

int pump1State = 0;
int pump2State = 0;
boolean heaterState = false;
boolean lightState = false;
float tubpowerCalc = 0;
double tubTemp = -1;
// Last non-temperature text shown on the panel display (error codes like OHH, dr, SnA)
String panelMsg = "";
unsigned long panelMsgSeen = 0;
double tubTargetTemp = -1;
String state = "unknown";
bool commandPending;

ArduinoQueue<String> sendBuffer(30);  // TODO: might be better bigger for large temp changes. Would need testing
unsigned long msgStartTime;
unsigned long timeSinceMsgStart;

String result = "";
String lastRaw = "";
String lastRaw2 = "";
String lastRaw3 = "";
String lastRaw4 = "";
String lastRaw5 = "";
String lastRaw6 = "";
String lastFB = "";
String lastJSON = "";
uint32_t lastUptime = 0;
String timeString = "";
int msgLength = 0;
boolean panelDetected = false;
const unsigned long readBytesTimeout = 2500; // Timeout in microseconds (2.5 ms)

// ---- command queue safety and watchdogs ----
// A command the controller never acknowledges used to be resent forever, and while the queue was not
// empty the page was not served at all, so the module looked frozen. Now:
#define CMD_TIMEOUT_MS 10000UL        // drop the queue if the first command gets no answer in 10 s
#define WEB_SLICE_MS 1000UL           // serve the page at least once a second, even while sending
#define WEB_WATCHDOG_MS 60000UL       // restart if the page was not served for 60 s
#define WIFI_WATCHDOG_MS 300000UL     // restart if WiFi stays down for 5 minutes
#define SINGLE_CMD_QUEUE_MAX 5        // page buttons are refused while this many commands wait
unsigned long headSince = 0;          // when the current first command started waiting
uint16_t headTries = 0;               // how many times it was sent
unsigned long lastWebServe = 0;
volatile uint32_t pin5Edges = 0;      // panel-select pulses, counted by the interrupt
TaskHandle_t rxFlushTask = NULL;      // woken by the pin 5 interrupt, clears the RX buffer
uint32_t pin5Rate = 0;                // pulses per second, measured every 5 s
bool sniffOn = false;                 // log button codes sent by the physical panel
String bootReason = "";
uint32_t bootCount = 0;
int pump1Lvl = 0;                     // 0 off, 1 low, 2 high, as reported by the controller
int pump2Lvl = 0;
uint32_t badStartBytes = 0;           // serial noise counters, summarised in the log once a minute
uint32_t readTimeouts = 0;

bool setTargetTemperature(float temperatureFloat);
void setOption(int currentIndex, int targetIndex, int options, String command);
String cmdName(const String& hex);
void dropHead();
int queueClear();
void restartWith(const String& why);
void attachPanelInterrupt();

#include "telnet.h"
#include "weblog.h"
#include "webui.h"
#include "webstatus.h"
#include "ota_online.h"

byte nibble(char c);
void hexCharacterStringToBytes(byte* byteArray, const char* hexString);
void handleMessage(size_t len, uint8_t buf[]);
String HexString2TimeString(String hexstring);
String HexString2ASCIIString(String hexstring);
void notePanelMsg(String hex);
String cleanAscii(String hex);
void sendCommand();
void sniffRead();
void notePanelButton(const uint8_t* p, const char* where);

void clearRXbuffer(void) {
        // clear the rx buffer
    #if defined(ESP32) || defined(RSC3)
    uart_flush(tubUART);
    #else
    // not tested but according to the code flush clears the buffer
    // https://github.com/plerup/espsoftwareserial/blob/main/src/SoftwareSerial.cpp#L434
    tub.flush();
    #endif
}

// Triggered when pin5 falling - ie our panel is selected
// clears serial receive buffer
// Kept tiny on purpose: it used to call uart_flush() here, which takes a lock and must not run inside an
// interrupt; the interrupt watchdog then restarted the ESP32 every few seconds. Clearing it later in loop()
// (v1.5.1) was too late: part of our message was already thrown away. Now the interrupt wakes a
// top-priority task on the same core, which clears the buffer within microseconds, outside the interrupt.
void rxFlushLoop(void* arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        clearRXbuffer();
    }
}

void IRAM_ATTR panelSelected() {
    msgStartTime = micros();
    pin5Edges++;
    BaseType_t woken = pdFALSE;
    if (rxFlushTask) vTaskNotifyGiveFromISR(rxFlushTask, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/**
 * @brief enable interrupt for pin5 falling level change so we can clear the rx buffer
 *  every time our panel is selected
 */
void attachPanelInterrupt() {
    attachInterrupt(digitalPinToInterrupt(PIN_5_PIN_DEF), panelSelected, FALLING);
}

// ---- helpers for the debug log, command queue and watchdogs ----

String cmdName(const String& hex) {
    String h = hex;
    h.toLowerCase();
    if (h == COMMAND_UP) return "Temp +";
    if (h == COMMAND_DOWN) return "Temp -";
    if (h == COMMAND_TIME) return "Ora";
    if (h == COMMAND_CHANGE_MODE) return "Mod";
    if (h == COMMAND_AUX) return "Bule";
    if (h == COMMAND_JET1) return "Jet 1";
    if (h == COMMAND_JET2) return "Jet 2";
    if (h == COMMAND_EMPTY) return "Gol";
    if (h == COMMAND_LIGHT) return "Lumina";
    return "cod necunoscut";
}

// Remove the first queued command (sent and accepted, or given up)
void dropHead() {
    if (!sendBuffer.isEmpty()) sendBuffer.dequeue();
    commandPending = false;
    headSince = 0;
    headTries = 0;
}

int queueClear() {
    int n = 0;
    while (!sendBuffer.isEmpty()) {
        sendBuffer.dequeue();
        n++;
    }
    commandPending = false;
    headSince = 0;
    headTries = 0;
    return n;
}

void restartWith(const String& why) {
    prefs.putString("rstWhy", why);
    logAdd('S', "Repornesc: " + why);
    logSaveFlash();
    delay(300);
    ESP.restart();
}

String resetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "pornire alimentare";
        case ESP_RST_EXT: return "reset extern";
        case ESP_RST_SW: return "repornire software";
        case ESP_RST_PANIC: return "eroare program (crash)";
        case ESP_RST_INT_WDT: return "watchdog (întreruperi blocate)";
        case ESP_RST_TASK_WDT: return "watchdog (program blocat)";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "trezire din somn";
        case ESP_RST_BROWNOUT: return "tensiune prea mică";
        default: return "necunoscut";
    }
}

// Button frames look like fb 06 aa bb cc 00 XX ~XX crc. XX = 0 is "no button".
String snLast = "";
unsigned long snLastMs = 0;
void notePanelButton(const uint8_t* p, const char* where) {
    if (p[0] != 0xFB || p[1] != 0x06) return;
    if ((uint8_t)(p[6] + p[7]) != 0xFF) return;
    if (p[6] == 0) return;
    String h = "";
    for (int i = 0; i < 9; i++) {
        if (p[i] < 0x10) h += '0';
        h += String(p[i], HEX);
    }
    // a held button repeats the same frame many times; log it once
    if (h == snLast && millis() - snLastMs < 1500) {
        snLastMs = millis();
        return;
    }
    snLast = h;
    snLastMs = millis();
    logAdd('P', "Panou fizic: " + cmdName(h) + " " + h + " (" + where + ")");
}

// While another panel has its turn on the bus, scan the bytes for its button frames
uint8_t snBuf[9];
uint8_t snLen = 0;
void sniffRead() {
    int guard = 64;
    while (tub.available() > 0 && guard-- > 0) {
        uint8_t b = tub.read();
        if (snLen == 0) {
            if (b == 0xFB) snBuf[snLen++] = b;
            continue;
        }
        snBuf[snLen++] = b;
        if (snLen == 2 && b != 0x06) {
            snLen = (b == 0xFB) ? 1 : 0;
            continue;
        }
        if (snLen == 9) {
            snLen = 0;
            notePanelButton(snBuf, "magistrală");
        }
    }
}

void watchdogTick() {
    unsigned long now = millis();

    // A command that never gets accepted: drop the whole queue (the rest depends on it, e.g. temperature steps)
    if (!sendBuffer.isEmpty()) {
        if (headSince == 0) {
            headSince = now;
        } else if (now - headSince > CMD_TIMEOUT_MS) {
            String h = sendBuffer.getHead();
            String why = headTries == 0 ? " (nu a venit rândul nostru, verifică pinul 5)" : " (jacuzzi-ul nu a răspuns)";
            int n = queueClear();
            logAdd('E', "Fără răspuns la " + cmdName(h) + " " + h + " după " + String(headTries) + " trimiteri" + why +
                            ", am golit coada (" + String(n) + ")");
        }
    }

    // Page not served for a long time: restart
    if (now - lastWebServe > WEB_WATCHDOG_MS) {
        restartWith("watchdog: pagina nu a răspuns 60 s");
    }

    static unsigned long lastSlow = 0;
    if (now - lastSlow < 1000) return;
    lastSlow = now;

    // WiFi down for 5 minutes: restart (the ESP32 also retries on its own meanwhile)
    static unsigned long wifiLostAt = 0;
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiLostAt == 0) {
            wifiLostAt = now ? now : 1;
            logAdd('E', "WiFi deconectat");
        } else if (now - wifiLostAt > WIFI_WATCHDOG_MS) {
            restartWith("watchdog: WiFi picat 5 minute");
        }
    } else if (wifiLostAt) {
        logAdd('S', "WiFi reconectat după " + String((now - wifiLostAt) / 1000) + " s");
        wifiLostAt = 0;
    }

    // Pin 5 pulses, checked every 5 s
    static unsigned long p5Check = 0;
    static bool p5Lost = false;
    if (now - p5Check >= 5000) {
        p5Check = now;
        uint32_t e = pin5Edges;
        pin5Edges = 0;
        uint32_t rate = e / 5;
        static unsigned long rateLogAt = 0;
        bool big = pin5Rate == 0 ? rate > 0 : (rate > pin5Rate * 3 / 2 || rate < pin5Rate * 2 / 3);
        if (rate > 0 && (rateLogAt == 0 || (big && now - rateLogAt > 60000))) {
            rateLogAt = now;
            logAdd(rate > 20000 ? 'E' : 'S', "Pin 5: " + String(rate) + " impulsuri/s" + (rate > 20000 ? " (prea multe, semnal cu zgomot?)" : ""));
        }
        pin5Rate = rate;
        if (panelDetected) {
            if (e == 0 && !p5Lost) {
                p5Lost = true;
                logAdd('E', "Pin 5: semnalul lipsește");
            } else if (e > 0 && p5Lost) {
                p5Lost = false;
                logAdd('S', "Pin 5: semnal revenit");
            }
        }
    }

    // Serial noise summary, once a minute
    static unsigned long noiseCheck = 0;
    if (now - noiseCheck >= 60000) {
        noiseCheck = now;
        if (badStartBytes || readTimeouts) {
            logAdd('E', "RS485 ultimul minut: " + String(badStartBytes) + " octeți necunoscuți, " + String(readTimeouts) +
                            " mesaje incomplete");
            badStartBytes = 0;
            readTimeouts = 0;
        }
    }
}

void sendCommand(String command, int count) {
    Serial.printf("Sending %s - %u times\n", command.c_str(), count);
    for (int i = 0; i < count; i++) {
        sendBuffer.enqueue(command.c_str());
    }
}

void setOption(int currentIndex, int targetIndex, int options, String command) {
    if (targetIndex > currentIndex) {
        sendCommand(command, (targetIndex - currentIndex));
    } else if (currentIndex != targetIndex) {
        int presses = (options - currentIndex) + targetIndex;
        sendCommand(command, presses);
    }
}

void onSwitchStateChanged(bool state, HASwitch* sender) {
    Serial.printf("Switch %s changed - ", sender->getName());
    if (state != lightState) {
        Serial.println("Toggle");
        sendBuffer.enqueue(COMMAND_LIGHT);
    } else {
        Serial.println("No change needed");
    }
}

void onPumpSwitchStateChanged(int8_t index, HASelect* sender) {
    Serial.printf("onPumpSwitchStateChanged %s %u\n", sender->getName(), index);
    int currentIndex = sender->getCurrentState();
    String command = COMMAND_JET1;
    int options = PUMP1_STATE_HIGH + 1;
    // getName() returns a C string, so compare contents (the original compared pointers)
    if (strcmp(sender->getName(), "Pump2") == 0) {
        command = COMMAND_JET2;
        options = PUMP2_STATE_HIGH + 1;
    }
    else if (strcmp(sender->getName(), "Aux") == 0) {
        command = COMMAND_AUX;
        options = AUX_STATE_HIGH + 1;
    }
    setOption(currentIndex, index, options, command);
}

void onModeSwitchStateChanged(int8_t index, HASelect* sender) {
    Serial.printf("Mode Switch changed - %u\n", index);
    int currentIndex = sender->getCurrentState();
    int options = 3;
    sendBuffer.enqueue(COMMAND_CHANGE_MODE);
    setOption(currentIndex, index, options, COMMAND_DOWN);
    sendBuffer.enqueue(COMMAND_CHANGE_MODE);
}

void onButtonPress(HAButton* sender) {
    String name = sender->getName();
    Serial.printf("Button press - %s\n", name);
    if (name == "Up") {
        sendBuffer.enqueue(COMMAND_UP);
    } else if (name == "Down") {
        sendBuffer.enqueue(COMMAND_DOWN);
    } else if (name == "Mode") {
        sendBuffer.enqueue(COMMAND_CHANGE_MODE);
    } else if (name == "Time") {
        sendBuffer.enqueue(COMMAND_TIME);
    } else {
        Serial.printf("Unknown button %s\n", name);
    }
}

// Returns false when the current target is not known yet. In that case one "up" press is queued:
// it only opens the set-temperature menu on the controller, which then reports the current target.
bool setTargetTemperature(float temperatureFloat) {
    Serial.print("Set target temperature: ");
    Serial.println(temperatureFloat);

    if (tubTargetTemp < 0) {
        Serial.println("ERROR: can't adjust target as current value not known");
        sendBuffer.enqueue(
            COMMAND_UP);  // Enter set temp mode - won't change, but should allow us to capture the set target value
        return false;
    }

    int target = temperatureFloat * 2;  // 0.5 inc so double
    int current = tubTargetTemp * 2;
    sendBuffer.enqueue(COMMAND_UP);  // Enter set temp mode

    if (temperatureFloat > tubTargetTemp) {
        for (int i = 0; i < (target - current); i++) {
            Serial.println("Raise the temp");
            sendBuffer.enqueue(COMMAND_UP);
        }
    } else {
        for (int i = 0; i < (current - target); i++) {
            Serial.println("Lower the temp");
            sendBuffer.enqueue(COMMAND_DOWN);
        }
    }

    // Don't report the new target back here - better to show what the control unit reports
    // than to assume our commands worked
    return true;
}

void onTargetTemperatureCommand(HANumeric temperature, HAHVAC* sender) {
    setTargetTemperature(temperature.toFloat());
}

void setPixel(uint8_t color) {
#ifdef RSC3X
    switch(color) {
        case 0:
            pixels.setPixelColor(0, pixels.Color(255,0,0));
            break;
        case 1:
            pixels.setPixelColor(0, pixels.Color(0,255,0));
            break;
        case 2:
            pixels.setPixelColor(0, pixels.Color(0,0,255));
            break;
        case 3:
            pixels.setPixelColor(0, pixels.Color(255,255,0));
            break;
        case 4:
            pixels.setPixelColor(0, pixels.Color(255,0,255));
            break;
    }     
    pixels.show();
#endif
}

void MQTTUpdate(void *pvParameters) {
    for (;;) {
        mqtt.loop();
        delay(100);
    }
}


boolean isConnected = false;
TaskHandle_t MQTTUpdateTask;
void setup() {
    Serial.begin(115200);
    delay(1000);

#ifdef RSC3X
    pixels.begin();
    pixels.setBrightness(255);
    setPixel(STATUS_BOOT);
#else    
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
#endif

    // Load optional MQTT settings saved by the setup page
    prefs.begin("jacuzzi", false);

    // Restart bookkeeping: count restarts and remember why the last one happened
    bootCount = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", bootCount);
    String why = prefs.getString("rstWhy", "");
    if (why.length() > 0) prefs.remove("rstWhy");
    esp_reset_reason_t rr = esp_reset_reason();
    bootReason = (rr == ESP_RST_SW && why.length() > 0) ? why : resetReasonText(rr);
    logBegin(bootCount, rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT);
    logAdd('S', "Pornire #" + String(bootCount) + ", motiv: " + bootReason + ", firmware " FW_VERSION);
    prefs.getString("mqttHost", "").toCharArray(mqttHost, sizeof(mqttHost));
    prefs.getString("mqttPort", "1883").toCharArray(mqttPort, sizeof(mqttPort));
    prefs.getString("mqttUser", "").toCharArray(mqttUser, sizeof(mqttUser));
    prefs.getString("mqttPass", "").toCharArray(mqttPass, sizeof(mqttPass));

    // WiFi: connect to the saved network, or open the "Jacuzzi-Setup" hotspot so the
    // network can be chosen from a phone. No passwords in the source code.
    WiFi.mode(WIFI_STA);
    WiFiManager wm;
    WiFiManagerParameter pMqttHost("mqtt_host", "MQTT broker (optional, empty = disabled)", mqttHost, sizeof(mqttHost) - 1);
    WiFiManagerParameter pMqttPort("mqtt_port", "MQTT port", mqttPort, sizeof(mqttPort) - 1);
    WiFiManagerParameter pMqttUser("mqtt_user", "MQTT user", mqttUser, sizeof(mqttUser) - 1);
    WiFiManagerParameter pMqttPass("mqtt_pass", "MQTT password", mqttPass, sizeof(mqttPass) - 1);
    wm.addParameter(&pMqttHost);
    wm.addParameter(&pMqttPort);
    wm.addParameter(&pMqttUser);
    wm.addParameter(&pMqttPass);
    bool saveParams = false;
    wm.setSaveParamsCallback([&saveParams]() { saveParams = true; });
    wm.setSaveConfigCallback([&saveParams]() { saveParams = true; });
    wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT);
    wm.setTitle("Jacuzzi");

    Serial.println("Connecting to WiFi (setup hotspot opens if needed)");
    if (!wm.autoConnect(SETUP_AP_NAME)) {
        Serial.print("\n\nWifi failed, reboot\n");
        delay(1000);
        ESP.restart();
    }

    if (saveParams) {
        prefs.putString("mqttHost", pMqttHost.getValue());
        prefs.putString("mqttPort", pMqttPort.getValue());
        prefs.putString("mqttUser", pMqttUser.getValue());
        prefs.putString("mqttPass", pMqttPass.getValue());
        strlcpy(mqttHost, pMqttHost.getValue(), sizeof(mqttHost));
        strlcpy(mqttPort, pMqttPort.getValue(), sizeof(mqttPort));
        strlcpy(mqttUser, pMqttUser.getValue(), sizeof(mqttUser));
        strlcpy(mqttPass, pMqttPass.getValue(), sizeof(mqttPass));
    }
    mqttEnabled = strlen(mqttHost) > 0;

    logAdd('S', "WiFi conectat, IP " + WiFi.localIP().toString());
    clockBegin();
    setPixel(STATUS_WIFI);

    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
    }

    pinMode(RTS_PIN_DEF, OUTPUT);
    Serial.printf("Setting pin %u LOW\n", RTS_PIN_DEF);
    digitalWrite(RTS_PIN_DEF, LOW);
    pinMode(PIN_5_PIN_DEF, INPUT);
#ifdef ESP32
    Serial.printf("Setting serial port as pins %u, %u\n", RX_PIN, TX_PIN);
    tub.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
    while (tub.available() > 0) {  // workarond for bug with hanging during Serial2.begin -
                                   // https://github.com/espressif/arduino-esp32/issues/5443
        Serial.read();
    }
    Serial.printf("Set serial port as pins %u, %u\n", RX_PIN,
                  TX_PIN);  // added here to see if line about was where the hang was
    tub.updateBaudRate(115200);
#endif

    xTaskCreatePinnedToCore(rxFlushLoop, "rxFlush", 2048, NULL, configMAX_PRIORITIES - 1, &rxFlushTask,
                            xPortGetCoreID());
    attachPanelInterrupt();

    webota
        .onStart([]() {
            logAdd('S', "Update firmware început");
            detachInterrupt(digitalPinToInterrupt(PIN_5_PIN_DEF));
        })
        .onEnd([]() {
            prefs.putString("rstWhy", "update firmware");
            logAdd('S', "Update firmware terminat, repornesc");
            logSaveFlash();
            attachPanelInterrupt();
        })
        .onError([](ota_error_t error) {
            Serial.printf("Error[%u]: ", error);
            if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
            else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
            else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
            else if (error == OTA_END_ERROR) Serial.println("End Failed");
            attachPanelInterrupt();
        });

    webota.init(8080, "/update");

    // start telnet server
    server.begin();
    server.setNoDelay(true);

    // start web
    webserver.on("/", handleRoot);
    webserver.on("/raw", handleRawStatus);
    webserver.on("/send", handleSend);
    webserver.on("/api/status", handleApiStatus);
    webserver.on("/api/cmd", handleApiCmd);
    webserver.on("/api/target", handleApiTarget);
    webserver.on("/api/mode", handleApiMode);
    webserver.on("/api/wifireset", HTTP_POST, handleApiWifiReset);
    webserver.on("/api/config", HTTP_GET, handleApiConfigGet);
    webserver.on("/api/config", HTTP_POST, handleApiConfigPost);
    webserver.on("/api/history", handleApiHistory);
    webserver.on("/api/log", handleApiLog);
    webserver.on("/api/time", HTTP_POST, handleApiTime);
    webserver.on("/api/ota", HTTP_POST, handleApiOta);
    webserver.on("/api/log/clear", HTTP_POST, handleApiLogClear);
    webserver.on("/api/log/note", HTTP_POST, handleApiLogNote);
    webserver.on("/api/log/sniff", HTTP_POST, handleApiSniff);
    webserver.on("/api/queue/clear", HTTP_POST, handleApiQueueClear);
    webserver.on("/api/restart", HTTP_POST, handleApiRestart);
    webserver.on("/api/sendhex", HTTP_POST, handleApiSendHex);
    webserver.on("/manifest.webmanifest", handleManifest);
    webserver.on("/icon-180.png", handleIcon180);
    webserver.on("/icon-192.png", handleIcon192);
    webserver.on("/icon-512.png", handleIcon512);
    webserver.begin();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    // Home Assistant
    device.setName("Hottub");
    device.setSoftwareVersion(FW_VERSION);
    device.setManufacturer("Balboa");
    device.setModel("GL2000");

    temp.setDeviceClass("temperature");
    temp.setName("Tub temperature");

    targetTemp.setDeviceClass("temperature");
    targetTemp.setName("Target Tub temp");

#ifndef USE_FAHRENHEIT
    temp.setUnitOfMeasurement("°C");
    targetTemp.setUnitOfMeasurement("°C");
#else
    temp.setUnitOfMeasurement("°F");
    targetTemp.setUnitOfMeasurement("°F");
#endif

    timeToTemp.setName("Time to temp");
    timeToTemp.setUnitOfMeasurement("min");
    timeToTemp.setDeviceClass("duration");
    timeToTemp.setIcon("mdi:clock");

    currentState.setName("Status");

    pump1.setName("Pump1");
#ifdef PUMP1_DUAL_SPEED
    pump1.setOptions("Off;Medium;High");
#else
    pump1.setOptions("Off;High");
#endif
    pump1.setIcon("mdi:chart-bubble");
    pump1.onCommand(onPumpSwitchStateChanged);

    pump2.setName("Pump2");
#ifdef PUMP2_DUAL_SPEED
    pump2.setOptions("Off;Medium;High");
#else
    pump2.setOptions("Off;High");
#endif
    pump2.setIcon("mdi:chart-bubble");
    pump2.onCommand(onPumpSwitchStateChanged);

    aux.setName("Aux");
#ifdef AUX_DUAL_SPEED
    aux.setOptions("Off;Medium;High");
#else
    aux.setOptions("Off;High");
#endif
    aux.setIcon("mdi:chart-bubble");
    aux.onCommand(onPumpSwitchStateChanged);


    heater.setName("Heater");
    heater.setIcon("mdi:radiator");
    light.setName("Light");
    light.onCommand(onSwitchStateChanged);

    haTime.setName("Time");

    rawData.setName("Raw data");
    rawData2.setName("Non-Temp");
    rawData3.setName("CMD");
    fbData.setName("FB");
    commandQueueSize.setName("Command Queue");
    timeSinceMsgStartSensor.setName("Command Time");

    tubMode.setName("Mode");
    tubMode.setOptions("Standard;Economy;Sleep");
    tubMode.onCommand(onModeSwitchStateChanged);

    uptime.setName("Uptime");
    uptime.setUnitOfMeasurement("seconds");
    uptime.setDeviceClass("duration");

    tubpower.setName("Tub Power");
    tubpower.setUnitOfMeasurement("kW");
    tubpower.setDeviceClass("power");

    btnUp.setName("Up");
    btnDown.setName("Down");
    btnMode.setName("Mode");
    btnTime.setName("Time");

    btnUp.onCommand(onButtonPress);
    btnDown.onCommand(onButtonPress);
    btnMode.onCommand(onButtonPress);
    btnTime.onCommand(onButtonPress);

    hvac.onTargetTemperatureCommand(onTargetTemperatureCommand);
    hvac.setModes(HAHVAC::AutoMode);
    hvac.setMode(HAHVAC::AutoMode, true);
    hvac.setName("Target Temp");
    hvac.setMinTemp(26);
    hvac.setMaxTemp(40);
    hvac.setTempStep(0.5);

    if (mqttEnabled) {
        uint16_t port = atoi(mqttPort);
        if (port == 0) port = 1883;
        Serial.printf("MQTT enabled, broker %s:%u\n", mqttHost, port);
        if (strlen(mqttUser) > 0) {
            mqtt.begin(mqttHost, port, mqttUser, mqttPass);
        } else {
            mqtt.begin(mqttHost, port);
        }
        xTaskCreatePinnedToCore(MQTTUpdate, "MQTTUpdate", 10000, NULL, 1, &MQTTUpdateTask, 1);
    } else {
        Serial.println("MQTT disabled (no broker configured)");
    }

#ifdef ESP32
    Serial.println("Configuring WDT...");
    esp_task_wdt_init(WDT_TIMEOUT, true);  // enable panic so ESP32 restarts
    esp_task_wdt_add(NULL);                // add current thread to WDT watch
#endif
    lastWebServe = millis();
    Serial.println("End of setup");
    digitalWrite(LED_BUILTIN, LOW);
}


/*
 * waitforGLBytes checks the first byte in the serial buffer
 * since we've cleared the serial buffer when pin5 went low
 * there will only be data meant for our panel in the buffer.
 * Depending on the message type, wait for the expected number
 * of bytes to be available with a timeout of 2ms
 * returns number of bytes to read
 */
int waitforGLBytes() {
    int msgLength = 0;
    setPixel(STATUS_OK);
    // define message length from starting Byte
    switch (tub.peek()) {
        case 0xFA:
            msgLength = 23;
            break;
        case 0xAE:
            msgLength = 16;
            break;
        case 0xFB:
            msgLength = 9;
            break;
        default:
            tub.read();
            badStartBytes++;
            return 0;
    }
    // we'll wait here for up to 2.5ms until the expected number of bytes are available
    unsigned long startTime = micros();
    while (tub.available() < msgLength) {
        if (micros() - startTime >= readBytesTimeout) {
            readTimeouts++;
            return 0;
        }
    }
    return msgLength;
}
void readSerial(bool panelSelect) {
    // is data available and we are selected
    if ((panelSelect == LOW) && (tub.available() > 0)) {
        int msgLength = waitforGLBytes();
        // only do something if we've got a message
        if (msgLength > 0) {
            uint8_t buf[msgLength];
            tub.read(buf, msgLength);
            handleMessage(msgLength, buf);
        }
    }
    else {
        if (sniffOn && panelSelect == HIGH) {
            sniffRead();  // another panel's turn: look for its button codes
        }
        if(panelDetected) {
            setPixel(STATUS_WAITING_DATA);
        }
        else {
            setPixel(STATUS_WAITING_PANEL);
        }
    }
}
void loop() {
    bool panelSelect = (GPIO.in >> PIN_5_PIN_DEF) & 0x1;  // LOW when we are meant to read data
    readSerial(panelSelect);
    commandQueueSize.setValue((u_int8_t) sendBuffer.itemCount());

    if (panelSelect == HIGH || !panelDetected) {  // Controller talking to other topside panels - we are in effect idle

        if(panelSelect == HIGH) {
            if (!panelDetected) logAdd('S', "Pin 5 detectat, jacuzzi-ul ne dă rândul la comenzi");
            panelDetected = true;
        }
        else {
            state = "Panel select (pin5) not detected";
            currentState.setValue(state.c_str());
        }

        webota.handle();
    
        telnetLoop();

        // The webserver and websocket can block for a while, so while commands wait they only get a short
        // turn once a second instead of never (never made the page unreachable when a command got stuck)
        if (sendBuffer.isEmpty() || !panelDetected || (millis() - lastWebServe >= WEB_SLICE_MS)) {
            lastWebServe = millis();
            webserver.handleClient();
            webSocket.loop();

            if (webSocket.connectedClients(false) > 0) {
                String json = getStatusJSON();
                if (json != lastJSON) {
                    webSocket.broadcastTXT(json);
                    lastJSON = json;
                }
            }
        }
    }

    if (((millis() / 1000) - lastUptime) >= 15) {
        lastUptime = millis() / 1000;
        uptime.setValue(lastUptime);
    }

    historyTick();
    watchdogTick();
    logTick();

#ifdef ESP32
    esp_task_wdt_reset();
#endif
}

void handleMessage(size_t len, uint8_t buf[]) {
    //      Serial.print("message = ");
    //      Serial.println(result);
    // check if we need to send command first
    if (buf[0] == 0xFA) {
        sendCommand();
    }
    result = "";
    for (int i = 0; i < len; i++) {
        if (buf[i] < 0x10) {
            result += '0';
        }
        result += String(buf[i], HEX);
    }
    if (result.substring(0, 4) == "fa14") {
        // Serial.println("FA 14");
        // telnetSend(result);

        // fa1433343043 = header + 340C = 34.0C

        // If messages is temp or ---- for temp, it is status message
        if (result.substring(10, 12) == "43" || result.substring(10, 12) == "46" || result.substring(10, 12) == "2d") {
            tubpowerCalc = 0;
            String pump = result.substring(13, 14);
            int pumpBits = (int)strtol(pump.c_str(), NULL, 16);
            pump1Lvl = pumpBits & 0x3;
            pump2Lvl = (pumpBits >> 2) & 0x3;

            if (pump == "0") {  // Pump 1 Off - Pump 2 Off - 0b0000
                pump1State = 0;
                pump2State = 0;
            } else if (pump == "1") {  // Pump 1 Low - Pump 2 Off - 0b0001
                pump1State = 1;
                pump2State = 0;
                tubpowerCalc += POWER_PUMP1_LOW;
            } else if (pump == "2") {  // Pump 1 High - Pump 2 Off - 0b0010
                pump1State = PUMP1_STATE_HIGH;
                pump2State = 0;
                tubpowerCalc += POWER_PUMP1_HIGH;
            } else if (pump == "4") {  // Pump 1 Off - Pump 2 Low - 0b0100
                pump1State = 0;
                pump2State = 1;
                tubpowerCalc += POWER_PUMP2_LOW;
            } else if (pump == "5") {  // Pump 1 Low - Pump 2 Low - 0b0101
                pump1State = 1;
                pump2State = 1;
                tubpowerCalc += POWER_PUMP1_LOW;
                tubpowerCalc += POWER_PUMP2_LOW;
            } else if (pump == "6") {  // Pump 1 High - Pump 2 Low - 0b0110
                pump1State = PUMP1_STATE_HIGH;
                pump2State = 1;
                tubpowerCalc += POWER_PUMP1_HIGH;
                tubpowerCalc += POWER_PUMP2_LOW;
            } else if (pump == "8") {  // Pump 1 Off - Pump 2 High - 0b1000
                pump1State = 0;
                pump2State = PUMP2_STATE_HIGH;
                tubpowerCalc += POWER_PUMP2_HIGH;
            } else if (pump == "9") {  // Pump 1 Low - Pump 2 High - 0b1001
                pump1State = 1;
                pump2State = PUMP2_STATE_HIGH;
                tubpowerCalc += POWER_PUMP1_LOW;
                tubpowerCalc += POWER_PUMP2_HIGH;
            } else if (pump == "a") {  // Pump 1 High - Pump 2 HIGH = 0b1010
                pump1State = PUMP1_STATE_HIGH;
                pump2State = PUMP2_STATE_HIGH;
                tubpowerCalc += POWER_PUMP1_HIGH;
                tubpowerCalc += POWER_PUMP2_HIGH;
            }

            String heater = result.substring(14, 15);
            if (heater == "0") {
                heaterState = false;
            } else if (heater == "1") {
                heaterState = true;
                tubpowerCalc += POWER_HEATER;
            } else if (heater == "2") {
                heaterState = true;  // heater off, verifying temp change, but creates noisy state if we return false
                tubpowerCalc += POWER_HEATER;
            }

            tubpower.setValue(tubpowerCalc);

            String light = result.substring(15, 16);
            if (light == "0") {
                lightState = false;
            } else if (light == "3") {
                lightState = true;
            }

            // Ignore last 2 bytes as possibly checksum, given we have temp earlier making look more complex than
            // perhaps it is
            String newRaw = result.substring(17, 44);
            if (lastRaw != newRaw) {
                lastRaw = newRaw;
                rawData.setValue(lastRaw.c_str());

                String s = result.substring(17, 18);
                if (s == "4") {
                    state = "Sleep";
                    tubMode.setState(MODE_IDX_SLP);
                } else if (s == "9") {
                    state = "Circulation";
                    tubMode.setState(MODE_IDX_STD);  // TODO: confirm
                } else if (s == "1") {
                    state = "Standard";
                    tubMode.setState(MODE_IDX_STD);
                } else if (s == "2") {
                    state = "Economy";
                    tubMode.setState(MODE_IDX_ECO);
                } else if (s == "a") {
                    state = "Cleaning";  // TODO: can't tell our actual mode here - could be any of the 3 I think
                } else if (s == "c") {
                    state = "Circulation in sleep";
                    tubMode.setState(MODE_IDX_SLP);
                } else if (s == "b" || s == "3") {
                    state = "Std in Eco";  // Was in eco, Swap to STD for 1 hour only
                    tubMode.setState(MODE_IDX_STD);
                } else {
                    state = "Unknown " + s;
                }

                String menu = result.substring(18, 20);
                if (menu == "00") {
                    // idle
                } else if (menu == "4c") {
                    state = "Set Mode";
                } else if (menu == "46") {
                    state = "Set Temperature";
                } else if (menu == "5a") {
                    state = "Standby?";  // WT: not tested to confirm if this is the act of setting Standby or just seen
                                         // when in standby
                } else {                 // 46 for set temp, but also other things like filter time
                    state = "menu " + menu;
                }

                // 94600008002ffffff0200000000f5

                if (result.substring(28, 32) != "ffff") {
                    timeString = HexString2TimeString(result.substring(28, 32));
                } else {
                    timeString = "--:--";
                }
                haTime.setValue(timeString.c_str());

                // temp up - ff0100000000?? - end varies

                // temp down - ff0200000000?? - end varies

                String cmd = result.substring(34, 44);
                if (cmd == "0000000000") {
                    // none
                } else if (cmd.substring(0, 4) == "01") {
                    state = "Temp Up";
                } else if (cmd.substring(0, 4) == "02") {
                    state = "Temp Down";
                } else {
                    telnetSend("CMD: " + cmd);
                }
                if (!lastRaw3.equals(cmd)) {
                    // Controller accepted command
                    if(commandPending && !sendBuffer.isEmpty()) {
                        String h = sendBuffer.getHead();
                        logAdd('T', "Acceptată " + cmdName(h) + " (" + String(headTries) + " trimiteri), răspuns cmd=" + cmd);
                        dropHead();
                        timeSinceMsgStartSensor.setValue((int) timeSinceMsgStart);
                    }
                    lastRaw3 = cmd;
                    if(cmd != "0000000000") {  // ignore idle command
                        rawData3.setValue(lastRaw3.c_str());
                    }
                }


                if (result.substring(10, 12) == "43" || result.substring(10, 12) == "46") {  // "C" or "F"
                    double tmp = (HexString2ASCIIString(result.substring(4, 10)).toDouble() / 10);
                    if (menu == "46") {
                        tubTargetTemp = tmp;
                        targetTemp.setValue((float)tubTargetTemp);
                        if (sendBuffer.isEmpty()) {  // supress setting the target while we are changing the target
                            hvac.setTargetTemperature((float)tubTargetTemp);
                        }
                        Serial.printf("Sent target temp data %f\n", tubTargetTemp);
                    } else {
                        if (tubTemp != tmp) {
                            tubTemp = tmp;
                            temp.setValue((float)tubTemp);
                            hvac.setCurrentCurrentTemperature((float)tubTemp);
                            Serial.printf("Sent temp data %f\n", tubTemp);
                        }
                        if (heaterState && (tubTemp < tubTargetTemp)) {
                            double tempDiff = (tubTargetTemp - tubTemp);
                            float timeToTempValue = (tempDiff * MINUTES_PER_DEGC);
                            timeToTemp.setValue(timeToTempValue);
                        } else {
                            timeToTemp.setValue((float) 0);
                        }
                    }
                } else if (result.substring(10, 12) == "2d") {  // "-"
                    notePanelMsg(result.substring(4, 12));
                                                                //          Serial.println("temp = unknown");
                                                                //          telnetSend("temp = unknown");
                } else {
                    Serial.println("non-temp " + result);
                    telnetSend("non-temp " + result);
                }

                if(heaterState) {
                    hvac.setMode(HAHVAC::HeatMode);
                }
                else if(tubMode.getCurrentState() == MODE_IDX_STD) {
                    hvac.setMode(HAHVAC::AutoMode);                    
                }
                else  {
                    hvac.setMode(HAHVAC::OffMode);
                }

                currentState.setValue(state.c_str());

                // Log status changes, but not the clock ticking every minute
                static String lastLogKey = "";
                String key = result.substring(4, 28) + result.substring(32, 46);
                if (key != lastLogKey) {
                    lastLogKey = key;
                    logAdd('R', "'" + cleanAscii(result.substring(4, 12)) + "' " + state + " | " + result.substring(4, 46));
                }
            }
        } else {
            // FA but not temp data - the display shows text instead (error codes, messages)
            notePanelMsg(result.substring(4, 12));
            if (lastRaw2 != result.substring(4, 28)) {
                logAdd('R', "Afișaj '" + cleanAscii(result.substring(4, 12)) + "' | " + result.substring(4, 46));
            }
            lastRaw2 = result.substring(4, 28);
            rawData2.setValue(lastRaw2.c_str());
        }

        if (result.length() >= 64) {  // "Long" messages only
            String tail = result.substring(46, 64);
            if (tail != lastFB) {
                lastFB = tail;
                fbData.setValue(lastFB.c_str());
            }
        }

        pump1.setState(pump1State);
        pump2.setState(pump2State);
        heater.setState(heaterState);
        light.setState(lightState);

        // end of FA14
    } else if (result.substring(0, 2) == "fb") {
        if (sniffOn) notePanelButton(buf, "slotul nostru");
        if (result != lastFB) {
            lastFB = result;
            fbData.setValue(lastFB.c_str());
        }
    } else if (result.substring(0, 4) == "ae0d") {
        // Serial.println("AE 0D");
        // telnetSend(result);

        String message = result.substring(0, 32);  // ignore any FB ending

        if (result.substring(0, 6) == "ae0d01" && message != "ae0d010000000000000000000000005a") {
            if (!lastRaw4.equals(message)) {
                lastRaw4 = message;
                // rawData4.setValue(lastRaw4.c_str());
            }
        } else if (result.substring(0, 6) == "ae0d02" && message != "ae0d02000000000000000000000000c3") {
            if (!lastRaw5.equals(message)) {
                lastRaw5 = message;
                // rawData5.setValue(lastRaw5.c_str());
            }
        } else if (result.substring(0, 6) == "ae0d03" && message != "ae0d03000000000000000000000000b4") {
            if (!lastRaw6.equals(message)) {
                lastRaw6 = message;
                // rawData6.setValue(lastRaw6.c_str());
            }
        }
        // end of AE 0D
    } else {
        static String lastUnknown = "";
        if (result != lastUnknown) {
            lastUnknown = result;
            logAdd('E', "Mesaj necunoscut: " + result.substring(0, 90));
        }
    }
}

unsigned long lastCmdTime = 0;
void sendCommand() {
    if (sendBuffer.isEmpty()) {
        return;
    }
    // Without the pin 5 signal we can't know when it is our turn; sending anyway talks over the controller
    if (!panelDetected) {
        return;
    }
    if((millis() - lastCmdTime) >= 400) {
        lastCmdTime = millis();
        commandPending = true;
        digitalWrite(RTS_PIN_DEF, HIGH);
        digitalWrite(LED_BUILTIN, HIGH);

        timeSinceMsgStart = micros() - msgStartTime;
        delayMicroseconds(delayTime);
        // Serial.println("Sending " + sendBuffer);
        byte byteArray[9] = {0};
        String head = sendBuffer.getHead();
        if (headTries == 0) logAdd('T', "Trimit " + cmdName(head) + " " + head);
        headTries++;
        hexCharacterStringToBytes(byteArray, head.c_str());
        tub.write(byteArray, sizeof(byteArray));
        if (digitalRead(PIN_5_PIN_DEF) != LOW) {
            logAdd('E', "Pin 5 s-a schimbat în timpul trimiterii, " + cmdName(head) + " pierdută");
            delayTime = DELAY_TIME_DEFAULT;
            dropHead();
        }
        // wait for tx to finish and flush the rx buffer
        tub.flush(true);
        if (digitalRead(PIN_5_PIN_DEF) == LOW) {
            // sendBuffer.dequeue(); // TODO: trying to resend now till we see response
            Serial.printf("Sent with delay of %u interval:%u\n", delayTime, timeSinceMsgStart);
            // delayTime += 10;
        }
        else {
          Serial.println("ERROR: Pin5 went high before command could be sent after flush");
        }
        delay(1);
        digitalWrite(RTS_PIN_DEF, LOW);
        digitalWrite(LED_BUILTIN, LOW);
    }
}

String HexString2TimeString(String hexstring) {
    // Convert "HHMM" in HEX to "HH:MM" with decimal representation
    String time = "";
    int hour = strtol(hexstring.substring(0, 2).c_str(), NULL, 16);
    int minute = strtol(hexstring.substring(2, 4).c_str(), NULL, 16);

    if (hour < 10) time.concat("0");  // Add leading zero
    time.concat(hour);
    time.concat(":");
    if (minute < 10) time.concat("0");  // Add leading zero
    time.concat(minute);

    return time;
}

// Remember display text that isn't a temperature, keeping only printable characters
String cleanAscii(String hex) {
    String raw = HexString2ASCIIString(hex);
    String clean = "";
    for (size_t i = 0; i < raw.length(); i++) {
        char c = raw[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') clean += c;
    }
    clean.trim();
    return clean;
}

void notePanelMsg(String hex) {
    String clean = cleanAscii(hex);
    if (clean.length() == 0) return;
    if (clean != panelMsg) {
        telnetSend("display: " + clean);
    }
    panelMsg = clean;
    panelMsgSeen = millis();
}

String HexString2ASCIIString(String hexstring) {
    String temp = "", sub = "", result;
    char buf[3];
    for (int i = 0; i < hexstring.length(); i += 2) {
        sub = hexstring.substring(i, i + 2);
        sub.toCharArray(buf, 3);
        char b = (char)strtol(buf, 0, 16);
        if (b == '\0') break;
        temp += b;
    }
    return temp;
}

void hexCharacterStringToBytes(byte* byteArray, const char* hexString) {
    bool oddLength = strlen(hexString) & 1;

    byte currentByte = 0;
    byte byteIndex = 0;

    for (byte charIndex = 0; charIndex < strlen(hexString); charIndex++) {
        bool oddCharIndex = charIndex & 1;

        if (oddLength) {
            // If the length is odd
            if (oddCharIndex) {
                // odd characters go in high nibble
                currentByte = nibble(hexString[charIndex]) << 4;
            } else {
                // Even characters go into low nibble
                currentByte |= nibble(hexString[charIndex]);
                byteArray[byteIndex++] = currentByte;
                currentByte = 0;
            }
        } else {
            // If the length is even
            if (!oddCharIndex) {
                // Odd characters go into the high nibble
                currentByte = nibble(hexString[charIndex]) << 4;
            } else {
                // Odd characters go into low nibble
                currentByte |= nibble(hexString[charIndex]);
                byteArray[byteIndex++] = currentByte;
                currentByte = 0;
            }
        }
    }
}

byte nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;  // Not a valid hexadecimal character
}
