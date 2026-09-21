/*
  ESP32 HOME AUTOMATION
  PlatformIO + Arduino

  Current stage:
  - DHT11
  - Digital LDR
  - Two relay-controlled LEDs
  - Two physical buttons
  - Buzzer
  - Firebase authenticated PING / PONG
  - ESP32 software-state publishing (reported/red, reported/green)
  - Authenticated Red/Green ON, OFF and AUTO cloud commands
  - Boot command baseline, deduplication and 15-second expiration
  - Logical physical-button priority over older commands
  - Live DHT11 / LDR telemetry, Wi-Fi RSSI and last heartbeat
  - Remote threshold + buzzer settings and 3 dashboard presets
  - On-device IST schedules (per channel; runs without web page open)
  - Explicit software command ACK and reported mode

  WARNING: only low-voltage relay/LED demonstration. No mains loads.
*/

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <DHT.h>
#include <time.h>
#include <sys/time.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "root_ca.h"
#include "cloud_paths.h"
// =====================================
// 1. PIN CONFIGURATION
// =====================================

#define DHT_PIN        4
#define LDR_PIN       16

#define RELAY_RED     18
#define RELAY_GREEN   19

#define BUTTON_RED    21
#define BUTTON_GREEN  22

#define BUZZER_PIN    23

DHT dht(DHT_PIN, DHT11);

// Match these settings to your successful
// individual relay and buzzer tests.

const bool RELAY_ACTIVE_LOW = true;
const bool BUZZER_ACTIVE_HIGH = true;

const int RELAY_ON =
    RELAY_ACTIVE_LOW ? LOW : HIGH;

const int RELAY_OFF =
    RELAY_ACTIVE_LOW ? HIGH : LOW;

const int BUZZER_ON =
    BUZZER_ACTIVE_HIGH ? HIGH : LOW;

const int BUZZER_OFF =
    BUZZER_ACTIVE_HIGH ? LOW : HIGH;

// Your verified digital LDR polarity:
// DARK = LOW; BRIGHT = HIGH.

const int LDR_DARK = LOW;


// =====================================
// 2. AUTOMATION SETTINGS
// =====================================

float tempOn = 30.0;
float tempOff = 28.0;

unsigned long ledBeepMs = 1000;
unsigned long tempBeepMs = 5000;

enum Mode {
    AUTO_MODE,
    MANUAL_MODE,
    SCHEDULE_MODE
};

Mode redMode = AUTO_MODE;
Mode greenMode = AUTO_MODE;

bool redState = false;
bool greenState = false;

// Keep declarations ABOVE setRed() / setGreen() so they compile.
// Publish the initial OFF states after Firebase connects.
bool redStatusPending = true;
bool greenStatusPending = true;
bool redModePending = true;
bool greenModePending = true;

// Last command acknowledged is the *software* action, not optical feedback.
struct AckState {
    bool pending = false;
    String requestId;
    String result;
};
AckState redAck;
AckState greenAck;

struct DaySchedule {
    bool configured = false;
    int onMinute = 8 * 60;
    int offMinute = 20 * 60;
    int daysMask = 127; // Bit 0 Sunday, bit 1 Monday, ... bit 6 Saturday.
};
DaySchedule redSchedule;
DaySchedule greenSchedule;

bool settingsLoaded = false;
unsigned long lastTelemetryAttempt = 0;
unsigned long lastAckAttempt = 0;
unsigned long lastModeAttempt = 0;
const unsigned long TELEMETRY_INTERVAL_MS = 10000;
unsigned long lastStatusAttempt = 0;

// Physical button timestamps are declared early because updateButtons() uses them.
int64_t lastRedPhysicalMs = 0;
int64_t lastGreenPhysicalMs = 0;
int64_t unixMillis();

bool darkDetected = false;

float temperature = NAN;
float humidity = NAN;

bool sensorHealthy = false;
bool sensorFault = false;

int failedReads = 0;
int recoveryReads = 0;


// =====================================
// 3. TIMERS
// =====================================

unsigned long lastDhtRead = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastCloudAttempt = 0;

const unsigned long DHT_INTERVAL = 2000;
const unsigned long WIFI_RETRY_MS = 15000;
const unsigned long CLOUD_RETRY_MS = 30000;


// =====================================
// 4. BUZZER
// =====================================

bool buzzerRunning = false;

unsigned long buzzerStarted = 0;
unsigned long buzzerDuration = 0;

void startBuzzer(unsigned long duration) {

    if (duration == 0) return;

    unsigned long now = millis();

    if (buzzerRunning) {

        unsigned long elapsed =
            now - buzzerStarted;

        unsigned long remaining =
            elapsed < buzzerDuration
                ? buzzerDuration - elapsed
                : 0;

        // Do not replace a longer alert
        // with a shorter alert.

        if (remaining >= duration) return;
    }

    buzzerStarted = now;
    buzzerDuration = duration;
    buzzerRunning = true;

    digitalWrite(BUZZER_PIN, BUZZER_ON);
}

void updateBuzzer() {

    if (!buzzerRunning) return;

    if (millis() - buzzerStarted >=
        buzzerDuration) {

        digitalWrite(BUZZER_PIN, BUZZER_OFF);

        buzzerRunning = false;
    }
}


// =====================================
// 5. RELAY CONTROL
// =====================================

void setRed(bool on) {

    if (redState == on) return;

    redState = on;

    // State has changed locally; Firebase must receive the update.
    redStatusPending = true;

    digitalWrite(
        RELAY_RED,
        on ? RELAY_ON : RELAY_OFF
    );

    Serial.println(
        on ? "RED: ON" : "RED: OFF"
    );

    startBuzzer(ledBeepMs);
}

void setGreen(bool on) {

    if (greenState == on) return;

    greenState = on;

    // State has changed locally; Firebase must receive the update.
    greenStatusPending = true;

    digitalWrite(
        RELAY_GREEN,
        on ? RELAY_ON : RELAY_OFF
    );

    Serial.println(
        on ? "GREEN: ON" : "GREEN: OFF"
    );

    startBuzzer(ledBeepMs);
}


// =====================================
// 6. BUTTON DEBOUNCE
// =====================================

struct Button {

    uint8_t pin;

    int lastRaw;
    int stable;

    unsigned long changedAt;
};

Button redButton = {
    BUTTON_RED, HIGH, HIGH, 0
};

Button greenButton = {
    BUTTON_GREEN, HIGH, HIGH, 0
};

const unsigned long DEBOUNCE_MS = 50;

bool wasPressed(Button &button) {

    int reading = digitalRead(button.pin);

    if (reading != button.lastRaw) {

        button.lastRaw = reading;
        button.changedAt = millis();
    }

    if (millis() - button.changedAt >=
        DEBOUNCE_MS) {

        if (reading != button.stable) {

            button.stable = reading;

            return reading == LOW;
        }
    }

    return false;
}

void updateButtons() {

    if (wasPressed(redButton)) {

        redMode = MANUAL_MODE;
        redModePending = true;
        lastRedPhysicalMs = unixMillis();

        setRed(!redState);

        Serial.println(
            "RED: MANUAL BUTTON"
        );
    }

    if (wasPressed(greenButton)) {

        greenMode = MANUAL_MODE;
        greenModePending = true;
        lastGreenPhysicalMs = unixMillis();

        setGreen(!greenState);

        Serial.println(
            "GREEN: MANUAL BUTTON"
        );
    }
}


// =====================================
// 7. LDR AUTOMATION
// =====================================

int lastLdrReading = HIGH;

unsigned long ldrChangedAt = 0;

const unsigned long LDR_FILTER_MS = 300;

void updateLDR() {

    int reading = digitalRead(LDR_PIN);

    if (reading != lastLdrReading) {

        lastLdrReading = reading;
        ldrChangedAt = millis();
    }

    if (millis() - ldrChangedAt <
        LDR_FILTER_MS) {

        return;
    }

    darkDetected =
        lastLdrReading == LDR_DARK;

    if (redMode == AUTO_MODE) {

        setRed(darkDetected);
    }
}


// =====================================
// 8. TEMPERATURE RANGE
// =====================================

enum TempRange {

    UNKNOWN_RANGE,
    LOW_RANGE,
    MIDDLE_RANGE,
    HIGH_RANGE
};

TempRange previousRange = UNKNOWN_RANGE;
TempRange candidateRange = UNKNOWN_RANGE;

int rangeCount = 0;

TempRange getTempRange(float t) {

    if (t <= tempOff) return LOW_RANGE;

    if (t >= tempOn) return HIGH_RANGE;

    return MIDDLE_RANGE;
}

void checkTemperatureAlert(float t) {

    TempRange nextRange = getTempRange(t);

    if (previousRange == UNKNOWN_RANGE) {

        previousRange = nextRange;
        candidateRange = nextRange;

        rangeCount = 0;

        return;
    }

    if (nextRange == previousRange) {

        candidateRange = nextRange;
        rangeCount = 0;

        return;
    }

    if (nextRange != candidateRange) {

        candidateRange = nextRange;
        rangeCount = 1;

        return;
    }

    rangeCount++;

    if (rangeCount >= 2) {

        previousRange = nextRange;

        rangeCount = 0;

        Serial.println(
            "TEMPERATURE RANGE CHANGED"
        );

        startBuzzer(tempBeepMs);
    }
}


// =====================================
// 9. TEMPERATURE AUTOMATION
// =====================================

void controlGreenAutomatically() {

    if (greenMode != AUTO_MODE) return;

    if (!sensorHealthy || sensorFault) {

        setGreen(false);
        return;
    }

    if (temperature >= tempOn) {

        setGreen(true);
    }

    else if (temperature <= tempOff) {

        setGreen(false);
    }

    // Between thresholds:
    // keep previous state.
}


// =====================================
// 10. DHT11 SENSOR
// =====================================

void updateDHT() {

    if (millis() - lastDhtRead <
        DHT_INTERVAL) {

        return;
    }

    lastDhtRead = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(t) || isnan(h)) {

        failedReads++;
        recoveryReads = 0;

        Serial.println("DHT11 READ FAILED");

        if (failedReads >= 3) {

            sensorFault = true;
            sensorHealthy = false;

            Serial.println(
                "DHT11 PERSISTENT ERROR"
            );

            controlGreenAutomatically();
        }

        return;
    }

    failedReads = 0;

    if (sensorFault) {

        recoveryReads++;

        if (recoveryReads < 3) {

            Serial.println(
                "DHT11 RECOVERY IN PROGRESS"
            );

            return;
        }

        sensorFault = false;
        previousRange = UNKNOWN_RANGE;

        Serial.println("DHT11 RECOVERED");
    }

    sensorHealthy = true;

    temperature = t;
    humidity = h;

    Serial.println();
    Serial.println("----- SENSOR DATA -----");

    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" C");

    Serial.print("Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");

    Serial.print("Light: ");
    Serial.println(
        darkDetected ? "DARK" : "BRIGHT"
    );

    Serial.println("-----------------------");

    controlGreenAutomatically();

    checkTemperatureAlert(temperature);
}



// =====================================
// V2. SCHEDULE ENGINE — device-local, IST (UTC+05:30)
// =====================================
// There is intentionally no replay of missed events: the relay follows
// the current window. If clock/config is unavailable, schedule output OFF.

bool scheduleIsOn(const DaySchedule &plan, time_t utcNow) {
    if (!plan.configured || utcNow < 1700000000) return false;
    const time_t india = utcNow + 19800; // India has no seasonal DST.
    struct tm local;
    gmtime_r(&india, &local);
    const int minute = local.tm_hour * 60 + local.tm_min;
    const int todayBit = 1 << local.tm_wday;
    const int yesterdayBit = 1 << ((local.tm_wday + 6) % 7);

    if (plan.onMinute < plan.offMinute) {
        return (plan.daysMask & todayBit) &&
               minute >= plan.onMinute && minute < plan.offMinute;
    }
    // Overnight schedule: e.g. Monday 20:00 -> Tuesday 06:00.
    return ((plan.daysMask & todayBit) && minute >= plan.onMinute) ||
           ((plan.daysMask & yesterdayBit) && minute < plan.offMinute);
}

void runSchedules() {
    static unsigned long lastTick = 0;
    if (millis() - lastTick < 1000UL) return;
    lastTick = millis();
    time_t now = time(nullptr);
    if (redMode == SCHEDULE_MODE) setRed(scheduleIsOn(redSchedule, now));
    if (greenMode == SCHEDULE_MODE) setGreen(scheduleIsOn(greenSchedule, now));
}

const char *modeName(Mode mode) {
    return mode == AUTO_MODE ? "AUTO" :
           mode == MANUAL_MODE ? "MANUAL" : "SCHEDULE";
}

void applySettingsPayload(const String &payload) {
    if (payload.length() == 0 || payload == "null") return;
    JsonDocument doc;
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) {
        Serial.println("Settings: invalid JSON");
        return;
    }
    // Repeat validation in firmware even though the server validates writes.
    if (!(doc["tempOn"].is<float>() || doc["tempOn"].is<int>()) ||
        !(doc["tempOff"].is<float>() || doc["tempOff"].is<int>()) ||
        !doc["ledBeepMs"].is<int>() || !doc["tempBeepMs"].is<int>()) return;
    const float high = doc["tempOn"].as<float>();
    const float low = doc["tempOff"].as<float>();
    const int beep = doc["ledBeepMs"].as<int>();
    const int alarm = doc["tempBeepMs"].as<int>();
    if (high < 20 || high > 45 || low < 15 || low > 44 ||
        high - low < 1 || beep < 0 || beep > 5000 ||
        alarm < 0 || alarm > 10000) return;

    DaySchedule plans[2] = {redSchedule, greenSchedule};
    const char *keys[2] = {"scheduleRed", "scheduleGreen"};
    for (int i = 0; i < 2; ++i) {
        JsonVariantConst plan = doc[keys[i]];
        if (!plan.is<JsonObjectConst>() ||
            !plan["onMinute"].is<int>() ||
            !plan["offMinute"].is<int>() ||
            !plan["daysMask"].is<int>()) return;
        plans[i].onMinute = plan["onMinute"].as<int>();
        plans[i].offMinute = plan["offMinute"].as<int>();
        plans[i].daysMask = plan["daysMask"].as<int>();
        if (plans[i].onMinute < 0 || plans[i].onMinute > 1439 ||
            plans[i].offMinute < 0 || plans[i].offMinute > 1439 ||
            plans[i].onMinute == plans[i].offMinute ||
            plans[i].daysMask < 1 || plans[i].daysMask > 127) return;
        plans[i].configured = true;
    }
    tempOn = high;
    tempOff = low;
    ledBeepMs = (unsigned long)beep;
    tempBeepMs = (unsigned long)alarm;
    redSchedule = plans[0];
    greenSchedule = plans[1];
    settingsLoaded = true;
    previousRange = UNKNOWN_RANGE; // Do not beep merely for a settings update.
    controlGreenAutomatically();
    runSchedules();
    Serial.println("V2: validated settings applied; schedules ready.");
}

// =====================================
// 11. FIREBASE OBJECTS
// =====================================

UserAuth deviceAuth(
    API_KEY,
    DEVICE_EMAIL,
    DEVICE_PASSWORD
);

FirebaseApp firebaseApp;

WiFiClientSecure sslClient;

AsyncClientClass firebaseClient(sslClient);

RealtimeDatabase database;

bool timeSyncStarted = false;
bool firebaseStarted = false;
bool pingPongCompleted = false;

// A new ESP32 boot never replays an existing cloud command.
// First successful read per output establishes the baseline only.
struct RemoteCommandSlot {
    bool baselineReceived = false;
    bool inFlight = false;
    String inFlightUid;
    String lastRequestId;
    int64_t lastIssuedAt = 0;
    uint32_t lastPoll = 0;
    uint32_t sentAt = 0;
};

RemoteCommandSlot redCloud;
RemoteCommandSlot greenCloud;
RemoteCommandSlot settingsCloud;
uint32_t requestSequence = 0;

const uint32_t COMMAND_POLL_INTERVAL_MS = 2500;
const uint32_t COMMAND_TIMEOUT_MS = 15000;
const int64_t COMMAND_MAX_AGE_MS = 15000;
const int64_t BUTTON_PRIORITY_WINDOW_MS = 500;

int64_t unixMillis() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

void handleRemotePayload(bool red, const String &payload) {
    RemoteCommandSlot &slot = red ? redCloud : greenCloud;
    String json = payload;
    json.trim();

    // A missing command is a valid, empty baseline.
    if (json.length() == 0 || json == "null") {
        slot.baselineReceived = true;
        return;
    }

    JsonDocument doc;
    DeserializationError parseError = deserializeJson(doc, json);
    if (parseError || !doc.is<JsonObject>()) {
        Serial.println(red ? "RED cloud: invalid JSON" : "GREEN cloud: invalid JSON");
        slot.baselineReceived = true;
        return;
    }

    const char *idText = doc["requestId"] | "";
    const char *modeText = doc["mode"] | "";
    String id(idText);
    String mode(modeText);
    if (id.length() == 0 || id.length() > 64 ||
        (mode != "MANUAL" && mode != "AUTO" && mode != "SCHEDULE") ||
        !doc["on"].is<bool>() || !doc["issuedAt"].is<int64_t>()) {
        Serial.println("Cloud command rejected: invalid fields");
        slot.baselineReceived = true;
        return;
    }
    const bool requestedOn = doc["on"].as<bool>();
    const int64_t issuedAt = doc["issuedAt"].as<int64_t>();

    if (!slot.baselineReceived) {
        slot.baselineReceived = true;
        slot.lastRequestId = id;
        slot.lastIssuedAt = issuedAt;
        Serial.println(red ? "RED cloud baseline stored; old command ignored"
                           : "GREEN cloud baseline stored; old command ignored");
        return;
    }

    if (id == slot.lastRequestId || issuedAt <= slot.lastIssuedAt) return;
    // Mark each new ID as seen even when rejected, so it is not reconsidered.
    slot.lastRequestId = id;
    slot.lastIssuedAt = issuedAt;

    int64_t now = unixMillis();
    if (now < 1700000000000LL || issuedAt > now + 5000LL ||
        now - issuedAt > COMMAND_MAX_AGE_MS) {
        Serial.println("Cloud command rejected: stale or future timestamp");
        return;
    }

    // Read physical inputs again immediately before applying a cloud command.
    updateButtons();
    int64_t physicalAt = red ? lastRedPhysicalMs : lastGreenPhysicalMs;
    if (physicalAt != 0 && issuedAt <= physicalAt + BUTTON_PRIORITY_WINDOW_MS) {
        Serial.println("Cloud command skipped: newer/conflicting physical button");
        AckState &ack = red ? redAck : greenAck;
        ack.requestId = id;
        ack.result = "SKIPPED";
        ack.pending = true;
        return;
    }

    if (red) {
        if (mode == "SCHEDULE") {
            redMode = SCHEDULE_MODE;
            setRed(scheduleIsOn(redSchedule, time(nullptr)));
        } else if (mode == "AUTO") {
            redMode = AUTO_MODE;
            setRed(darkDetected); // 'on' is ignored in AUTO.
        } else {
            redMode = MANUAL_MODE;
            setRed(requestedOn);
        }
    } else {
        if (mode == "SCHEDULE") {
            greenMode = SCHEDULE_MODE;
            setGreen(scheduleIsOn(greenSchedule, time(nullptr)));
        } else if (mode == "AUTO") {
            greenMode = AUTO_MODE;
            controlGreenAutomatically();
        } else {
            greenMode = MANUAL_MODE;
            setGreen(requestedOn);
        }
    }
    if (red) {
        redModePending = true;
        redAck.requestId = id;
        redAck.result = "APPLIED";
        redAck.pending = true;
    } else {
        greenModePending = true;
        greenAck.requestId = id;
        greenAck.result = "APPLIED";
        greenAck.pending = true;
    }
    Serial.println(red ? "RED remote command accepted" : "GREEN remote command accepted");
}

// =====================================
// 12. FIREBASE CALLBACK
// =====================================

void firebaseCallback(AsyncResult &result) {
    if (!result.isResult()) return;
    String uid = result.uid();
    const bool isRed = (uid == redCloud.inFlightUid && redCloud.inFlight);
    const bool isGreen = (uid == greenCloud.inFlightUid && greenCloud.inFlight);
    const bool isSettings = (uid == settingsCloud.inFlightUid && settingsCloud.inFlight);

    if (result.isError()) {
        Serial.print("Firebase error: ");
        Serial.println(result.error().message());
        if (isRed) redCloud.inFlight = false;
        if (isGreen) greenCloud.inFlight = false;
        if (isSettings) settingsCloud.inFlight = false;
        return;
    }
    if (!result.available()) return;

    if (isRed) {
        redCloud.inFlight = false;
        handleRemotePayload(true, String(result.c_str()));
    } else if (isGreen) {
        greenCloud.inFlight = false;
        handleRemotePayload(false, String(result.c_str()));
    } else if (isSettings) {
        settingsCloud.inFlight = false;
        applySettingsPayload(String(result.c_str()));
    }
}

// Non-blocking FirebaseClient read, one request for each output every 2.5 s.
void pollOneCommand(bool red) {
    if (!firebaseStarted || !firebaseApp.ready() || WiFi.status() != WL_CONNECTED) return;
    RemoteCommandSlot &slot = red ? redCloud : greenCloud;
    uint32_t now = millis();
    if (slot.inFlight) {
        if (now - slot.sentAt < COMMAND_TIMEOUT_MS) return;
        // Ignore a late response from a timed-out task by changing task UID.
        slot.inFlight = false;
        slot.inFlightUid = "";
        Serial.println("Cloud command read timed out; retrying");
    }
    if (now - slot.lastPoll < COMMAND_POLL_INTERVAL_MS) return;
    slot.lastPoll = now;
    slot.sentAt = now;
    slot.inFlightUid = String(red ? "cmdRed_" : "cmdGreen_") + String(++requestSequence);
    slot.inFlight = true;
    database.get(firebaseClient, red ? RED_COMMAND_PATH : GREEN_COMMAND_PATH,
                 firebaseCallback, false, slot.inFlightUid);
}


// =====================================
// V2. READ PERSISTENT SETTINGS ASYNCHRONOUSLY
// =====================================
void pollSettings() {
    if (!firebaseStarted || !firebaseApp.ready() || WiFi.status() != WL_CONNECTED) return;
    const unsigned long now = millis();
    if (settingsCloud.inFlight) {
        if (now - settingsCloud.sentAt < COMMAND_TIMEOUT_MS) return;
        settingsCloud.inFlight = false;
        settingsCloud.inFlightUid = "";
    }
    if (settingsCloud.lastPoll && now - settingsCloud.lastPoll < 10000UL) return;
    settingsCloud.lastPoll = now;
    settingsCloud.sentAt = now;
    settingsCloud.inFlightUid = String("settings_") + String(++requestSequence);
    settingsCloud.inFlight = true;
    database.get(firebaseClient, SETTINGS_PATH, firebaseCallback,
                 false, settingsCloud.inFlightUid);
}

// =====================================
// 13. WI-FI MANAGEMENT
// =====================================

void updateWiFi() {

    static bool connectedPreviously = false;
    bool connectedNow = (WiFi.status() == WL_CONNECTED);

    if (connectedNow && !connectedPreviously) {
        redStatusPending = true;
        greenStatusPending = true;
        redModePending = true;
        greenModePending = true;
        lastTelemetryAttempt = 0;
        // Recover the ping/pong test if Firebase was offline on startup.
        pingPongCompleted = false;
        Serial.println("Wi-Fi connected; states queued for synchronization.");
    }
    connectedPreviously = connectedNow;

    if (connectedNow) {

        if (!timeSyncStarted) {

            configTime(
                0,
                0,
                "pool.ntp.org",
                "time.google.com"
            );

            timeSyncStarted = true;

            Serial.println(
                "Synchronizing clock..."
            );
        }

        return;
    }

    if (millis() - lastWifiRetry >=
        WIFI_RETRY_MS) {

        lastWifiRetry = millis();

        Serial.println("Retrying Wi-Fi...");

        WiFi.reconnect();
    }
}


// =====================================
// 14. INITIALIZE FIREBASE
// =====================================

void startFirebaseWhenReady() {

    if (firebaseStarted) return;

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Wait for an approximately valid
    // internet-synchronized clock.

    if (time(nullptr) < 1700000000) {
        return;
    }

    Serial.println("Clock synchronized.");

    // Keep TLS certificate verification on.
    // Root compatibility must be checked
    // during our testing stage.

    sslClient.setCACert(ROOT_CA);

    initializeApp(
        firebaseClient,
        firebaseApp,
        getAuth(deviceAuth),
        firebaseCallback,
        "firebase_auth"
    );

    firebaseApp.getApp<RealtimeDatabase>(
        database
    );

    database.url(DATABASE_URL);

    firebaseStarted = true;

    Serial.println(
        "Firebase initialization started."
    );
}


// =====================================
// 15. FIREBASE PING / PONG
// =====================================

void updateFirebase() {

    if (!firebaseStarted) return;

    // Maintain authentication and token
    // renewal even after the first test.

    firebaseApp.loop();

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!firebaseApp.ready()) return;

    if (pingPongCompleted) return;

    if (lastCloudAttempt != 0 &&
        millis() - lastCloudAttempt <
        CLOUD_RETRY_MS) {

        return;
    }

    lastCloudAttempt = millis();

    Serial.println("Reading Firebase PING...");

    String command = database.get<String>(
        firebaseClient,
        "/homeAutomation/commands/test"
    );

    if (firebaseClient.lastError().code() != 0) {

        Serial.print("PING read failed: ");

        Serial.println(
            firebaseClient.lastError().message()
        );

        return;
    }

    command.trim();

    // Handle both plain and JSON-quoted text.
    if (command == "\"ping\"") {
        command = "ping";
    }

    if (command != "ping") {

        Serial.println(
            "Expected PING not found."
        );

        return;
    }

    Serial.println("PING received!");

    bool success = database.set<String>(
        firebaseClient,
        "/homeAutomation/reported/test",
        String("pong")
    );

    if (!success) {

        Serial.print("PONG write failed: ");

        Serial.println(
            firebaseClient.lastError().message()
        );

        return;
    }

    pingPongCompleted = true;

    Serial.println(
        "PONG successfully written!"
    );
}


// =====================================
// 16. PUBLISH ESP32-REPORTED LED STATES
// =====================================
// This reports software-commanded relay states, NOT independent
// physical LED feedback. Only DEVICE_UID can write reported/*.
// One synchronous write per attempt; retries retain pending flags.

void publishLEDStatus() {

    if (!firebaseStarted || !firebaseApp.ready()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (!redStatusPending && !greenStatusPending) return;
    if (millis() - lastStatusAttempt < 2000UL) return;

    lastStatusAttempt = millis();

    if (redStatusPending) {
        bool success = database.set<bool>(
            firebaseClient,
            RED_STATUS_PATH,
            redState
        );

        if (success) {
            redStatusPending = false;
            Serial.println("Red status published!");
        } else {
            Serial.print("Red status error: ");
            Serial.println(firebaseClient.lastError().message());
        }
        return;  // Green gets its own attempt on a later loop.
    }

    if (greenStatusPending) {
        bool success = database.set<bool>(
            firebaseClient,
            GREEN_STATUS_PATH,
            greenState
        );

        if (success) {
            greenStatusPending = false;
            Serial.println("Green status published!");
        } else {
            Serial.print("Green status error: ");
            Serial.println(firebaseClient.lastError().message());
        }
    }
}



// =====================================
// V2. TELEMETRY + MODE + EXPLICIT SOFTWARE ACK
// =====================================
// Each call writes at most one Firebase object. Errors retry on later loops.
void publishV2() {
    if (!firebaseStarted || !firebaseApp.ready() || WiFi.status() != WL_CONNECTED) return;
    if (time(nullptr) < 1700000000) return;
    unsigned long now = millis();

    if ((redAck.pending || greenAck.pending) && now - lastAckAttempt >= 1500UL) {
        lastAckAttempt = now;
        AckState &ack = redAck.pending ? redAck : greenAck;
        bool red = redAck.pending;
        JsonDocument doc;
        doc["requestId"] = ack.requestId;
        doc["result"] = ack.result;
        doc["appliedAt"] = unixMillis();
        String json;
        serializeJson(doc, json);
        if (database.set<object_t>(firebaseClient,
                red ? RED_ACK_PATH : GREEN_ACK_PATH, object_t(json))) {
            ack.pending = false;
            Serial.println(red ? "RED ACK published" : "GREEN ACK published");
        } else {
            Serial.print("ACK error: ");
            Serial.println(firebaseClient.lastError().message());
        }
        return;
    }

    if ((redModePending || greenModePending) && now - lastModeAttempt >= 1500UL) {
        lastModeAttempt = now;
        const bool red = redModePending;
        if (database.set<String>(firebaseClient,
                red ? RED_MODE_PATH : GREEN_MODE_PATH,
                String(modeName(red ? redMode : greenMode)))) {
            if (red) redModePending = false;
            else greenModePending = false;
        } else {
            Serial.print("Mode report error: ");
            Serial.println(firebaseClient.lastError().message());
        }
        return;
    }

    if (lastTelemetryAttempt && now - lastTelemetryAttempt < TELEMETRY_INTERVAL_MS) return;
    lastTelemetryAttempt = now;
    JsonDocument doc;
    // A failed DHT read is represented as invalid rather than made-up data.
    bool valid = sensorHealthy && !sensorFault && failedReads == 0;
    doc["valid"] = valid;
    doc["temperature"] = valid ? temperature : 0.0;
    doc["humidity"] = valid ? humidity : 0.0;
    doc["dark"] = darkDetected;
    doc["rssi"] = WiFi.RSSI();
    doc["uptimeSec"] = millis() / 1000UL;
    doc["updatedAt"] = unixMillis();
    String json;
    serializeJson(doc, json);
    if (!database.set<object_t>(firebaseClient, SENSOR_PATH, object_t(json))) {
        Serial.print("Telemetry publish error: ");
        Serial.println(firebaseClient.lastError().message());
    }
}

// =====================================
// 17. SERIAL COMMANDS
// =====================================

void processCommand(String command) {

    command.trim();
    command.toUpperCase();

    if (command == "RED AUTO") {

        redMode = AUTO_MODE;
        redModePending = true;
        setRed(darkDetected);

        Serial.println("RED AUTO ENABLED");
    }

    else if (command == "GREEN AUTO") {

        greenMode = AUTO_MODE;
        greenModePending = true;
        controlGreenAutomatically();

        Serial.println("GREEN AUTO ENABLED");
    }

    else if (command == "RED ON") {

        redMode = MANUAL_MODE;
        redModePending = true;
        setRed(true);
    }

    else if (command == "RED OFF") {

        redMode = MANUAL_MODE;
        redModePending = true;
        setRed(false);
    }

    else if (command == "GREEN ON") {

        greenMode = MANUAL_MODE;
        greenModePending = true;
        setGreen(true);
    }

    else if (command == "GREEN OFF") {

        greenMode = MANUAL_MODE;
        greenModePending = true;
        setGreen(false);
    }

    else if (command == "RED SCHEDULE") {
        redMode = SCHEDULE_MODE;
        redModePending = true;
        setRed(scheduleIsOn(redSchedule, time(nullptr)));
    }
    else if (command == "GREEN SCHEDULE") {
        greenMode = SCHEDULE_MODE;
        greenModePending = true;
        setGreen(scheduleIsOn(greenSchedule, time(nullptr)));
    }
    else if (command == "STATUS") {

        Serial.println("--- STATUS ---");

        Serial.print("RED: ");
        Serial.println(redState ? "ON" : "OFF");

        Serial.print("GREEN: ");
        Serial.println(greenState ? "ON" : "OFF");

        Serial.print("Wi-Fi: ");
        Serial.println(
            WiFi.status() == WL_CONNECTED
                ? "CONNECTED"
                : "DISCONNECTED"
        );

        Serial.print("Firebase PING/PONG: ");
        Serial.println(
            pingPongCompleted
                ? "PASSED"
                : "NOT COMPLETE"
        );
    }

    else {

        Serial.println("Unknown command.");
    }
}

void checkSerial() {

    if (!Serial.available()) return;

    String command =
        Serial.readStringUntil('\n');

    processCommand(command);
}


// =====================================
// 17. SETUP
// =====================================

void setup() {

    Serial.begin(115200);
    Serial.setTimeout(30);

    // ESP32 Arduino core 3.x requires pinMode before digitalWrite.
    // This removes the "IO is not set as GPIO" warnings.
    // Test only with low-voltage LEDs; this does not prevent
    // every possible relay glitch during power-on/reset.
    pinMode(RELAY_RED, OUTPUT);
    pinMode(RELAY_GREEN, OUTPUT);
    digitalWrite(RELAY_RED, RELAY_OFF);
    digitalWrite(RELAY_GREEN, RELAY_OFF);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, BUZZER_OFF);

    pinMode(LDR_PIN, INPUT);

    pinMode(BUTTON_RED, INPUT_PULLUP);
    pinMode(BUTTON_GREEN, INPUT_PULLUP);

    redButton.lastRaw =
        digitalRead(BUTTON_RED);

    redButton.stable = redButton.lastRaw;

    greenButton.lastRaw =
        digitalRead(BUTTON_GREEN);

    greenButton.stable = greenButton.lastRaw;

    dht.begin();

    lastLdrReading = digitalRead(LDR_PIN);
    ldrChangedAt = millis();

    lastDhtRead = millis();

    Serial.println();
    Serial.println("=========================");
    Serial.println("ESP32 HOME AUTOMATION");
    Serial.println("=========================");

    WiFi.mode(WIFI_STA);

    WiFi.setAutoReconnect(true);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    Serial.println("Connecting Wi-Fi...");
}


// =====================================
// 18. MAIN LOOP
// =====================================

void loop() {

    // Physical inputs first.
    updateButtons();

    // Local automation works independently
    // of Firebase connectivity.
    updateLDR();
    updateDHT();
    runSchedules();
    updateBuzzer();

    checkSerial();

    // Internet functions.
    updateWiFi();
    startFirebaseWhenReady();
    updateFirebase();
    // Schedule remote reads without waiting for network responses.
    pollOneCommand(true);
    pollOneCommand(false);
    pollSettings();

    publishLEDStatus();
    publishV2();
}