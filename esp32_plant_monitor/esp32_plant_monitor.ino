/*
 * ESP32 Plant Monitor REST API (v3)
 *
 * Endpoints:
 * GET  /                 - device info
 * GET  /health            - liveness check
 * GET  /status            - current sensor snapshot (includes schedule state)
 * GET  /history           - recent buffered readings
 * POST /light?state=on|off&key=API_KEY       - manual light control (auth required)
 * GET  /schedule                              - view current schedule config
 * POST /schedule?enabled=true|false&key=API_KEY - enable/disable automatic schedule
 *
 * Grow light schedule (new in v3):
 *  - Automatic 8h-on / 16h-off cycle, driven by NTP time (see LIGHT_ON_HOUR /
 *    LIGHT_ON_DURATION_HOURS below). Checked once a minute in loop().
 *  - Fail-safe: if NTP time is unavailable (WiFi/NTP outage, or right after
 *    boot before the first sync lands), the light defaults to ON rather
 *    than risking hours of darkness.
 *  - A manual POST /light call will be overridden at the next schedule
 *    check (within ~60s) unless you first disable the schedule via
 *    POST /schedule?enabled=false&key=...
 *  - Set GMT_OFFSET_SEC for your timezone - it's currently PST (-8h, no DST).
 *
 * Changes from v2:
 *  - DHT11 reads throttled (min 2s between reads), cached in between
 *  - Soil ADC averaged over multiple samples to reduce noise
 *  - Non-blocking WiFi reconnect in loop(), NTP re-syncs after reconnect
 *  - API key check on /light and /schedule (query param)
 *  - In-memory ring buffer + /history endpoint for backlog in pull mode
 *  - Light state persisted across reboot using Preferences (NVS)
 *  - NTP-driven grow light schedule with fail-safe-on behavior
 *
 * NOTE: move WIFI_PASSWORD and API_KEY out of source before sharing/
 * committing this file anywhere public.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Preferences.h>
#include <time.h>

// =========================
// WiFi
// =========================
const char *WIFI_SSID = "WIFI-9EA4";
const char *WIFI_PASSWORD = "create8605award"; // TODO: move to a separate untracked header or NVS before sharing this file

// =========================
// API key for control endpoints
// =========================
const char *API_KEY = "123456"; // required as ?key=... on POST /light

// =========================
// Pins
// =========================
#define SOIL_PIN 32
#define LIGHT_PIN 16
#define DHT_PIN 4
#define DHT_TYPE DHT11

// =========================
// Soil Calibration
// =========================
const int SOIL_DRY = 3261;
const int SOIL_WET = 1199;
const int SOIL_SAMPLES = 10; // number of ADC reads averaged per soil reading

// =========================
// Timing
// =========================
const unsigned long DHT_MIN_INTERVAL_MS = 2500;   // don't read DHT11 faster than this
const unsigned long SAMPLE_INTERVAL_MS = 15000;   // how often to log a reading into history
const int HISTORY_SIZE = 60;                       // 60 samples * 15s = ~15 min of backlog

// =========================
// Grow light schedule (NTP-based, 8h on / 16h off)
// =========================
const char *NTP_SERVER = "pool.ntp.org";
const char *NTP_SERVER_BACKUP1 = "time.google.com";
const char *NTP_SERVER_BACKUP2 = "time.cloudflare.com";
const long GMT_OFFSET_SEC = -8 * 3600;    // TODO: set for your timezone (this is PST, no DST)
const int DAYLIGHT_OFFSET_SEC = 0;         // set to 3600 if you want automatic DST handling

const int LIGHT_ON_HOUR = 8;               // light turns on at 08:00 local time
const int LIGHT_ON_DURATION_HOURS = 8;     // stays on for 8 hours (off at 16:00)

const unsigned long SCHEDULE_CHECK_INTERVAL_MS = 60000; // check the schedule once a minute

// =========================
// Globals
// =========================
DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);
Preferences prefs;

bool lightState = false;

float cachedTemp = NAN;
float cachedHumidity = NAN;
unsigned long lastDhtRead = 0;

unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 5000;

unsigned long lastSample = 0;
unsigned long lastScheduleCheck = 0;

bool scheduleEnabled = true; // toggle via POST /schedule if you want to disable automatic control
bool timeIsSynced = false;   // updated each schedule check, exposed in /status
bool ntpStarted = false;     // tracks whether configTime() has been called since last connect

struct Reading {
    unsigned long t;   // seconds since boot
    float temp;
    float humidity;
    int soilPercent;
    bool lightOn;
};

Reading history[HISTORY_SIZE];
int historyCount = 0;
int historyHead = 0; // index where next reading will be written

// =========================
// Sensor helpers
// =========================

int readSoilPercent()
{
    long sum = 0;
    for (int i = 0; i < SOIL_SAMPLES; i++) {
        sum += analogRead(SOIL_PIN);
        delay(5);
    }
    int raw = sum / SOIL_SAMPLES;

    int percent = map(raw, SOIL_DRY, SOIL_WET, 0, 100);
    return constrain(percent, 0, 100);
}

// Refreshes temp/humidity cache only if enough time has passed since last read.
// DHT11 cannot reliably be read faster than ~1-2s; this also means hitting
// /status rapidly won't hammer the sensor or return NaN from over-polling.
void refreshDhtIfNeeded()
{
    unsigned long now = millis();
    if (now - lastDhtRead < DHT_MIN_INTERVAL_MS) {
        return; // keep cached values
    }

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    // Only overwrite cache on a valid read; keep last good value otherwise
    if (!isnan(t)) cachedTemp = t;
    if (!isnan(h)) cachedHumidity = h;

    lastDhtRead = now;
}

void addHistorySample(int soilPercent)
{
    Reading r;
    r.t = millis() / 1000;
    r.temp = cachedTemp;
    r.humidity = cachedHumidity;
    r.soilPercent = soilPercent;
    r.lightOn = lightState;

    history[historyHead] = r;
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;
}

// =========================
// WiFi
// =========================

void ensureWifiConnected()
{
    if (WiFi.status() == WL_CONNECTED) return;

    unsigned long now = millis();
    if (now - lastWifiCheck < WIFI_CHECK_INTERVAL_MS) return;
    lastWifiCheck = now;

    Serial.println("WiFi disconnected, attempting reconnect...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// =========================
// Grow light schedule
// =========================

void initTime()
{
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, NTP_SERVER_BACKUP1, NTP_SERVER_BACKUP2);
}

void applyLightState(bool on, bool persist)
{
    lightState = on;
    digitalWrite(LIGHT_PIN, lightState ? HIGH : LOW);
    if (persist) prefs.putBool("lightState", lightState);
}

// Returns true and fills `desired` if NTP time is available.
// Returns false if time hasn't been synced yet (e.g. WiFi/NTP outage).
bool desiredLightStateFromSchedule(bool &desired)
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 200)) { // 200ms timeout, only called once/minute
        return false;
    }

    int hour = timeinfo.tm_hour;
    int hoursSinceOn = (hour - LIGHT_ON_HOUR + 24) % 24;
    desired = hoursSinceOn < LIGHT_ON_DURATION_HOURS;
    return true;
}

// Checked once a minute from loop(). Applies the 8h-on/16h-off schedule,
// unless scheduleEnabled is false (manual-only mode).
void runScheduleCheck()
{
    if (!scheduleEnabled) return;

    bool desired;
    bool gotTime = desiredLightStateFromSchedule(desired);

    bool wasSynced = timeIsSynced;
    timeIsSynced = gotTime;

    if (gotTime && !wasSynced) {
        struct tm timeinfo;
        getLocalTime(&timeinfo, 200);
        Serial.printf("NTP time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    if (!gotTime) {
        Serial.println("NTP time not yet available (still waiting on sync)");

        // Fail-safe: if we don't know what time it is (WiFi down, NTP
        // unreachable, or we just booted and haven't synced yet), err on
        // the side of keeping the light ON so the plant isn't left in the
        // dark for hours by an outage. This only forces a change once -
        // it won't fight a state you already have.
        if (!lightState) {
            Serial.println("No NTP time available - failing safe to light ON");
            applyLightState(true, true);
        }
        return;
    }

    if (desired != lightState) {
        Serial.printf("Schedule transition: light -> %s\n", desired ? "ON" : "OFF");
        applyLightState(desired, true);
    }
}

// =========================
// HTTP handlers
// =========================

void handleRoot()
{
    JsonDocument doc;

    doc["device"] = "Desk Plant";
    doc["version"] = "2.0.0";
    doc["status"] = "online";

    String response;
    serializeJsonPretty(doc, response);
    server.send(200, "application/json", response);
}

void handleHealth()
{
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleStatus()
{
    refreshDhtIfNeeded();
    int soil = readSoilPercent();

    JsonDocument doc;

    doc["deviceId"] = "desk-plant-01";
    doc["uptime"] = millis() / 1000;

    JsonObject soilObj = doc["soil"].to<JsonObject>();
    soilObj["percentage"] = soil;

    JsonObject env = doc["environment"].to<JsonObject>();
    if (isnan(cachedTemp)) {
        env["temperature"] = nullptr;
    } else {
        env["temperature"] = cachedTemp;
    }
    if (isnan(cachedHumidity)) {
        env["humidity"] = nullptr;
    } else {
        env["humidity"] = cachedHumidity;
    }

    JsonObject light = doc["light"].to<JsonObject>();
    light["state"] = lightState;

    JsonObject sched = doc["schedule"].to<JsonObject>();
    sched["enabled"] = scheduleEnabled;
    sched["on_hour"] = LIGHT_ON_HOUR;
    sched["off_hour"] = (LIGHT_ON_HOUR + LIGHT_ON_DURATION_HOURS) % 24;
    sched["time_synced"] = timeIsSynced;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ip"] = WiFi.localIP().toString();
    wifi["rssi"] = WiFi.RSSI();

    String response;
    serializeJsonPretty(doc, response);
    server.send(200, "application/json", response);
}

void handleHistory()
{
    JsonDocument doc;
    JsonArray arr = doc["readings"].to<JsonArray>();

    // Walk the ring buffer from oldest to newest
    int start = (historyCount < HISTORY_SIZE) ? 0 : historyHead;
    for (int i = 0; i < historyCount; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        Reading &r = history[idx];

        JsonObject o = arr.add<JsonObject>();
        o["t"] = r.t;
        if (isnan(r.temp)) o["temp_c"] = nullptr; else o["temp_c"] = r.temp;
        if (isnan(r.humidity)) o["humidity"] = nullptr; else o["humidity"] = r.humidity;
        o["soil_pct"] = r.soilPercent;
        o["light_on"] = r.lightOn;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleLight()
{
    if (!server.hasArg("key") || server.arg("key") != API_KEY) {
        server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
        return;
    }

    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"missing state\"}");
        return;
    }

    String state = server.arg("state");

    if (state == "on") {
        applyLightState(true, true);
    } else if (state == "off") {
        applyLightState(false, true);
    } else {
        server.send(400, "application/json", "{\"error\":\"invalid state\"}");
        return;
    }

    // Note: a manual toggle here will be overridden at the next schedule
    // check (within 60s) if scheduleEnabled is true. Disable the schedule
    // via POST /schedule?enabled=false&key=... if you want manual control
    // to stick.

    JsonDocument doc;
    doc["success"] = true;
    doc["state"] = lightState;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleScheduleGet()
{
    JsonDocument doc;
    doc["enabled"] = scheduleEnabled;
    doc["on_hour"] = LIGHT_ON_HOUR;
    doc["off_hour"] = (LIGHT_ON_HOUR + LIGHT_ON_DURATION_HOURS) % 24;
    doc["time_synced"] = timeIsSynced;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleScheduleSet()
{
    if (!server.hasArg("key") || server.arg("key") != API_KEY) {
        server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
        return;
    }

    if (!server.hasArg("enabled")) {
        server.send(400, "application/json", "{\"error\":\"missing enabled\"}");
        return;
    }

    scheduleEnabled = server.arg("enabled") == "true";

    JsonDocument doc;
    doc["success"] = true;
    doc["enabled"] = scheduleEnabled;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleNotFound()
{
    server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// =========================
// Setup / Loop
// =========================

void setup()
{
    Serial.begin(115200);

    pinMode(LIGHT_PIN, OUTPUT);

    prefs.begin("plant-monitor", false);
    lightState = prefs.getBool("lightState", false);
    digitalWrite(LIGHT_PIN, lightState ? HIGH : LOW);

    dht.begin();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi not connected yet, will keep retrying in loop()");
    }
    // NTP sync (and re-sync after any reconnect) is kicked off from loop()

    server.on("/", HTTP_GET, handleRoot);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/history", HTTP_GET, handleHistory);
    server.on("/light", HTTP_POST, handleLight);
    server.on("/schedule", HTTP_GET, handleScheduleGet);
    server.on("/schedule", HTTP_POST, handleScheduleSet);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("REST API Started");
}

void loop()
{
    ensureWifiConnected();
    server.handleClient();

    if (WiFi.status() == WL_CONNECTED) {
        if (!ntpStarted) {
            initTime(); // (re)start NTP sync after boot or a reconnect
            ntpStarted = true;
        }
    } else {
        ntpStarted = false; // force a re-sync once we're back online
    }

    unsigned long now = millis();

    if (now - lastSample >= SAMPLE_INTERVAL_MS) {
        refreshDhtIfNeeded();
        int soil = readSoilPercent();
        addHistorySample(soil);
        lastSample = now;
    }

    if (now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL_MS) {
        runScheduleCheck();
        lastScheduleCheck = now;
    }
}
