/**
 * @file Master.ino
 * @brief PD Stepper Master firmware – controls the master stepper motor and
 *        proxies commands to a networked slave PD Stepper board.
 * @version 1.0
 *
 * @details
 * The master board performs two roles simultaneously:
 *  -# Runs its own TMC2209 stepper motor via velocity and open-loop position
 *     control, identical to the single-motor PD_Stepper_Web_Server sketch.
 *  -# Acts as an HTTP reverse-proxy for the slave board: browser commands
 *     addressed to `/slave/...` are queued in the async web-server callback
 *     and forwarded to the slave via HTTPClient inside the main `loop()`.
 *
 * **Network topology**
 * @startuml
 * !theme plain
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * skinparam componentBorderColor #555
 *
 * actor Browser
 *
 * package "WiFi AP: \"PD Stepper Master\"\n(192.168.4.0/24)" {
 *   component "Master\nESP32-S3\n192.168.4.1" as Master #fc4903
 *   component "Slave\nESP32-S3\n192.168.4.x" as Slave #4f9cf8
 * }
 *
 * Browser   --> Master : "HTTP GET/POST\n(port 80)"
 * Master    --> Browser : "HTML + status JSON"
 * Slave     --> Master : "POST /register\n(on boot)"
 * Master    --> Slave  : "HTTPClient\nPOST /update, /save\nGET /voltage, /position …"
 * @enduml
 *
 * **How to use**
 * 1. Flash this sketch to the **MASTER** PD Stepper board.
 * 2. Flash Slave.ino to the **SLAVE** PD Stepper board.
 * 3. Power on both boards; the slave connects to the master AP and registers.
 * 4. Connect a browser to WiFi `"PD Stepper Master"`, then visit `192.168.4.1`.
 *
 * **Required libraries**
 * - ESPAsyncWebServer: https://github.com/ESP32Async/ESPAsyncWebServer
 * - AsyncTCP:          https://github.com/ESP32Async/AsyncTCP
 * - TMC2209:           https://github.com/janelia-arduino/TMC2209
 *
 * For more info visit https://thingsbyjosh.com
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>  // https://github.com/ESP32Async/ESPAsyncWebServer
#include <AsyncTCP.h>           // https://github.com/ESP32Async/AsyncTCP
#include <TMC2209.h>            // https://github.com/janelia-arduino/TMC2209
#include <Preferences.h>
#include <HTTPClient.h>
#include "master_index_html.h"

Preferences preferences;

/** @brief WiFi Access Point SSID broadcast by the master. */
const char *ssid     = "PD Stepper Master";
/** @brief WiFi Access Point password (empty = open network). */
const char *password = "";

/** @brief Async HTTP server listening on port 80. */
AsyncWebServer server(80);

// TMC2209 stepper driver
TMC2209 stepper_driver;
HardwareSerial &serial_stream     = Serial2;
const long      SERIAL_BAUD_RATE  = 115200;
const uint8_t   RUN_CURRENT_PERCENT = 100;

/**
 * @defgroup MasterPins Pin definitions (ESP32-S3 GPIO)
 * @{
 */
// TMC2209 stepper driver
#define TMC_EN  21  ///< TMC2209 enable (LOW = enabled)
#define STEP     5  ///< Step pulse output
#define DIR      6  ///< Direction output
#define MS1      1  ///< Microstep config bit 0
#define MS2      2  ///< Microstep config bit 1
#define SPREAD   7  ///< SpreadCycle select
#define TMC_TX  17  ///< UART TX to TMC2209
#define TMC_RX  18  ///< UART RX from TMC2209
#define DIAG    16  ///< Stall / diagnostic input
#define INDEX   11  ///< Index pulse input

// USB-PD trigger (CH224K)
#define PG   15  ///< Power-good signal (LOW = good)
#define CFG1 38  ///< PD voltage config bit 0
#define CFG2 48  ///< PD voltage config bit 1
#define CFG3 47  ///< PD voltage config bit 2

// Misc
#define VBUS  4  ///< VBUS ADC input
#define NTC   7  ///< NTC thermistor ADC input
#define LED1 10  ///< Status LED 1
#define LED2 12  ///< Status LED 2 (mirrors DIAG/stall)
#define SW1  35  ///< Button 1 – decrease speed
#define SW2  36  ///< Button 2 – reset / stop
#define SW3  37  ///< Button 3 – increase speed
#define AUX1 14  ///< Auxiliary connector TX
#define AUX2 13  ///< Auxiliary connector RX
/** @} */

// AS5600 Hall-effect encoder (I2C)
#include <Wire.h>
#define AS5600_ADDRESS 0x36  ///< I2C address of AS5600 magnetic encoder

/** @brief Accumulated encoder count across multiple full rotations. */
signed long   total_encoder_counts = 0;
/** @brief Timestamp of last encoder read (ms). */
unsigned long lastEncRead           = 0;
/** @brief Scheduled-task period in ms (10 ms = 100 Hz). */
int           mainFreq              = 10;

// ── Global state ──────────────────────────────────────────────────────────────
/** @brief Current velocity setpoint forwarded to TMC2209. */
int  set_speed    = 0;
/** @brief Latest sampled state of the USB-PD power-good pin. */
bool PGState      = 0;
/** @brief Tracks whether the TMC2209 driver is currently enabled. */
bool enabledState = 0;
/** @brief Toggles with each STEP pulse for 50% duty cycle. */
bool state        = 0;

// Button debounce
bool incButtonState   = HIGH;  ///< Last stable state of SW3 (increase)
bool decButtonState   = HIGH;  ///< Last stable state of SW1 (decrease)
bool resetButtonState = HIGH;  ///< Last stable state of SW2 (reset)
unsigned long lastDebounceTime = 0;          ///< Last debounce timestamp (ms)
const unsigned long debounceDelay = 50;      ///< Debounce window in ms
int buttonSpeed = 0;                         ///< Current button-driven velocity

// Voltage reading
float VBusVoltage = 0;                         ///< Last computed VBUS voltage (V)
const float VREF      = 3.3;                   ///< ESP32 ADC reference voltage
const float DIV_RATIO = 0.1189427313;          ///< 20 kΩ / 2.7 kΩ divider ratio

/**
 * @defgroup MasterSettings Persistent settings (stored in NVS flash)
 * These strings are loaded from flash on boot and written back on each save.
 * @{
 */
String enabled1       = "enabled";  ///< "enabled" or "disabled"
String setVoltage     = "12";       ///< USB-PD voltage in V (5/9/12/15/20)
String microsteps     = "32";       ///< TMC2209 microstep resolution
String current        = "30";       ///< Run current as % (0–100)
String stallThreshold = "10";       ///< StallGuard threshold (0–255)
String standstillMode = "NORMAL";   ///< TMC2209 standstill mode
/** @} */

/**
 * @defgroup MasterPending Async-to-loop pending flags
 * ESPAsyncWebServer callbacks run on a FreeRTOS task separate from loop().
 * Commands are staged here and consumed safely inside loop().
 * @{
 */
volatile bool speedUpdatePending = false;  ///< New master velocity ready
volatile int  pendingSpeed       = 0;      ///< Velocity to apply (microstep units)
volatile bool posUpdatePending   = false;  ///< New master position step ready
volatile int  pendingPosMode     = 0;      ///< 1=--large, 2=-small, 3=+small, 4=++large
/** @} */

// Open-loop position control
signed long   setPoint        = 0;  ///< Desired position in microstep counts
signed long   CurrentPosition = 0;  ///< Current tracked position in microstep counts
unsigned long lastStep        = 0;  ///< Timestamp of last STEP pulse (µs)

/**
 * @defgroup SlaveManagement Slave connection state
 * @{
 */
/** @brief IP address string of the registered slave (empty if none). */
String slaveIP        = "";
/** @brief True when a slave has successfully registered and is reachable. */
bool   slaveConnected = false;

/** @brief Timestamp of last slave status poll (ms). */
unsigned long lastSlavePoll = 0;
/** @brief How often (ms) to poll the slave for fresh status values. */
const unsigned long slavePollInterval = 2000;

// Cached slave status (refreshed by pollSlave() in main loop)
String slaveCachedVoltage   = "N/A";         ///< Last polled VBUS voltage
String slaveCachedPosition  = "N/A";         ///< Last polled encoder position
String slaveCachedStatus    = "Not Connected"; ///< Last polled TMC2209 status
String slaveCachedPowergood = "N/A";         ///< Last polled power-good state

// Pending slave motor commands
volatile bool slaveSpeedPending = false;  ///< Slave velocity command queued
volatile int  slavePendingSpeed = 0;      ///< Slave velocity to forward
volatile bool slavePosPending   = false;  ///< Slave position step queued
volatile int  slavePendingPos   = 0;      ///< Slave position step mode to forward

// Pending slave settings (built from /slave/save POST, sent in loop)
volatile bool slaveSettingsPending = false;  ///< Slave settings ready to forward
String slaveSettingsEnabled    = "";         ///< enable field to send
String slaveSettingsVoltage    = "";         ///< voltage field to send
String slaveSettingsMicrosteps = "";         ///< microsteps field to send
String slaveSettingsCurrent    = "";         ///< current field to send
String slaveSettingsStall      = "";         ///< stall_threshold field to send
String slaveSettingsStandstill = "";         ///< standstill_mode field to send
/** @} */

// ── Forward declarations ──────────────────────────────────────────────────────
void readEncoder();
void configureSettings();
void readSettings();
void writeSettings();

// ── Status-reading helpers ────────────────────────────────────────────────────

/**
 * @brief Reads the CH224K power-good pin and returns a human-readable string.
 * @return "Power Good" when PG pin is LOW; "Power Bad" otherwise.
 */
String readPGState() {
  PGState = digitalRead(PG);
  return (PGState == 0) ? "Power Good" : "Power Bad";
}

/**
 * @brief Averages 10 ADC readings from the VBUS divider and computes line voltage.
 * @return Formatted string e.g. @c "12.04V"
 */
String readVoltage() {
  uint32_t mvSum = 0;
  for (int i = 0; i < 10; i++) mvSum += analogReadMilliVolts(VBUS);
  VBusVoltage = ((float)mvSum / 10.0 / 1000.0) / DIV_RATIO;
  return String(VBusVoltage, 2) + "V";
}

/**
 * @brief Calls readEncoder() and returns the total accumulated encoder count.
 * @return Decimal string of #total_encoder_counts
 */
String readEncoderPos() {
  readEncoder();
  return String(total_encoder_counts);
}

/**
 * @brief Queries the TMC2209 driver status register for fault conditions.
 * @return One of: "Hardware Disabled", "Over Temp Warning",
 *         "Over Temp Shutdown", or "No Errors".
 */
String readTMCStatus() {
  if (stepper_driver.hardwareDisabled()) return "Hardware Disabled";
  TMC2209::Status s = stepper_driver.getStatus();
  if (s.over_temperature_warning)  return "Over Temp Warning";
  if (s.over_temperature_shutdown) return "Over Temp Shutdown";
  return "No Errors";
}

/**
 * @brief Reads the TMC2209 StallGuard result register.
 * @return Decimal string of the raw StallGuard value (0 = stalled or very slow).
 */
String readStallStatus() {
  return String(stepper_driver.getStallGuardResult());
}

/**
 * @brief ESPAsyncWebServer template processor – substitutes `%VAR%` placeholders
 *        in #master_index_html with the current setting values.
 * @param var  Placeholder name (without `%` delimiters).
 * @return Replacement string, or empty string if the placeholder is unknown.
 */
String processor(const String &var) {
  if (var == "enabled1")        return (enabled1 == "enabled") ? "checked" : "";
  if (var == "microsteps")      return microsteps;
  if (var == "voltage")         return setVoltage;
  if (var == "current")         return current;
  if (var == "stall_threshold") return stallThreshold;
  if (var == "standstill_mode") return standstillMode;
  return "";
}

// ── Slave HTTP communication ──────────────────────────────────────────────────

/**
 * @brief Issues a blocking HTTP GET to the slave and returns the response body.
 *
 * Returns immediately with `"N/A"` when no slave is connected or its IP is
 * unknown.  Called only from the main loop (never from an async callback) to
 * avoid re-entrancy issues with HTTPClient.
 *
 * @param path  URL path on the slave, e.g. `"/voltage"`.
 * @return Response body string, or `"N/A"` on any error.
 *
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * participant "Master loop()" as Master
 * participant "Slave HTTP\nserver" as Slave
 *
 * Master  -> Master  : slaveConnected && !slaveIP.isEmpty()?
 * alt connected
 *   Master  ->  Slave  : GET http://<slaveIP><path>
 *   Slave   --> Master : 200 OK  body
 *   Master  --> Master : return body
 * else not connected
 *   Master  --> Master : return "N/A"
 * end
 * @enduml
 */
String slaveGet(const String &path) {
  if (!slaveConnected || slaveIP.isEmpty()) return "N/A";
  HTTPClient http;
  http.begin("http://" + slaveIP + path);
  http.setTimeout(800);
  int    code  = http.GET();
  String reply = (code == 200) ? http.getString() : "N/A";
  http.end();
  return reply;
}

/**
 * @brief Issues a blocking HTTP POST to the slave.
 *
 * Returns immediately with `false` when no slave is connected.
 * Called only from the main loop.
 *
 * @param path  URL path on the slave, e.g. `"/update"`.
 * @param body  URL-encoded POST body, e.g. `"slider=120"`.
 * @return `true` on HTTP 200, `false` otherwise.
 */
bool slavePost(const String &path, const String &body) {
  if (!slaveConnected || slaveIP.isEmpty()) return false;
  HTTPClient http;
  http.begin("http://" + slaveIP + path);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(800);
  int code = http.POST(body);
  http.end();
  return (code == 200);
}

/**
 * @brief Polls all slave status endpoints and updates the cached strings.
 *
 * Called from `loop()` every #slavePollInterval ms.  If both voltage and
 * status return `"N/A"` the slave is considered unreachable and
 * #slaveConnected is set to `false`.
 *
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * participant "Master loop()" as M
 * participant "Slave" as S
 *
 * M -> S : GET /voltage
 * S --> M : "12.01V"
 * M -> S : GET /position
 * S --> M : "4096"
 * M -> S : GET /status
 * S --> M : "No Errors"
 * M -> S : GET /powergood
 * S --> M : "Power Good"
 * note over M
 *   If voltage AND status == "N/A":
 *   slaveConnected = false
 * end note
 * @enduml
 */
void pollSlave() {
  slaveCachedVoltage   = slaveGet("/voltage");
  slaveCachedPosition  = slaveGet("/position");
  slaveCachedStatus    = slaveGet("/status");
  slaveCachedPowergood = slaveGet("/powergood");
  // If we get N/A for all, mark slave as disconnected
  if (slaveCachedVoltage == "N/A" && slaveCachedStatus == "N/A") {
    slaveConnected = false;
    slaveCachedStatus = "Not Connected";
  }
}

// ── Arduino setup ─────────────────────────────────────────────────────────────

/**
 * @brief One-time initialisation: GPIO, TMC2209, WiFi AP, and HTTP routes.
 *
 * **Initialisation sequence**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * start
 * :Configure USB-PD trigger pins (12 V default);
 * :Configure GPIO (buttons, LEDs, STEP/DIR);
 * :Configure TMC2209 pins and UART;
 * :Init I2C for AS5600 encoder;
 * :Set ADC attenuation for VBUS pin;
 * :readSettings() – load NVS flash;
 * :stepper_driver.setup() + disable;
 * :configureSettings() – apply loaded values;
 * :Serial.begin(115200);
 * :WiFi.softAP() – create "PD Stepper Master" AP;
 * :Register all HTTP routes;
 * :server.begin();
 * :Flash LED1 (setup complete indicator);
 * stop
 * @enduml
 */
void setup() {
  // USB-PD trigger pins
  pinMode(PG,   INPUT);
  pinMode(CFG1, OUTPUT);
  pinMode(CFG2, OUTPUT);
  pinMode(CFG3, OUTPUT);
  //                              5V   9V   12V  15V  20V
  digitalWrite(CFG1, LOW);  //    1    0    0    0    0
  digitalWrite(CFG2, LOW);  //    -    0    0    1    1
  digitalWrite(CFG3, HIGH); //    -    0    1    1    0   → 12 V default

  // General I/O
  pinMode(SW1,  INPUT);
  pinMode(SW2,  INPUT);
  pinMode(SW3,  INPUT);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(STEP, OUTPUT);
  pinMode(DIR,  OUTPUT);

  // TMC2209
  pinMode(MS1,    OUTPUT);
  pinMode(MS2,    OUTPUT);
  pinMode(TMC_EN, OUTPUT);
  pinMode(DIAG,   INPUT);
  digitalWrite(TMC_EN, LOW);
  digitalWrite(MS2,    LOW);

  // AS5600 encoder over I2C
  Wire.begin(SDA, SCL);

  // ADC for VBUS voltage sense
  analogSetPinAttenuation(VBUS, ADC_11db);

  readSettings();

  stepper_driver.setup(serial_stream, SERIAL_BAUD_RATE,
                       TMC2209::SERIAL_ADDRESS_0, TMC_RX, TMC_TX);
  stepper_driver.setRunCurrent(RUN_CURRENT_PERCENT);
  stepper_driver.enableAutomaticCurrentScaling();
  stepper_driver.enableStealthChop();
  stepper_driver.setCoolStepDurationThreshold(5000);
  stepper_driver.disable();
  configureSettings();

  delay(200); // Allow bootloader to settle before Serial.begin
  Serial.begin(115200);
  Serial.println("Master starting...");

  // Start WiFi Access Point
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // ── Web-server routes ────────────────────────────────────────────────────

  // Main UI page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", master_index_html, processor);
  });

  // Master status endpoints
  server.on("/powergood", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", readPGState());
  });
  server.on("/voltage", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", readVoltage());
  });
  server.on("/position", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", readEncoderPos());
  });
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", readTMCStatus());
  });
  server.on("/stallguard", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", readStallStatus());
  });

  // Master motor control
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("slider", true)) {
      pendingSpeed       = req->getParam("slider", true)->value().toInt();
      speedUpdatePending = true;
    }
    if (req->hasParam("positionControl", true)) {
      pendingPosMode   = req->getParam("positionControl", true)->value().toInt();
      posUpdatePending = true;
    }
    req->send(200);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    enabled1 = req->hasParam("enabled1", true) ? "enabled" : "disabled";
    if (req->hasParam("setvoltage", true)) {
      setVoltage = req->getParam("setvoltage", true)->value();
      stepper_driver.moveAtVelocity(0);
    }
    if (req->hasParam("microsteps", true)) {
      microsteps = req->getParam("microsteps", true)->value();
      stepper_driver.moveAtVelocity(0);
    }
    if (req->hasParam("current", true))
      current = req->getParam("current", true)->value();
    if (req->hasParam("stall_threshold", true))
      stallThreshold = req->getParam("stall_threshold", true)->value();
    if (req->hasParam("standstill_mode", true))
      standstillMode = req->getParam("standstill_mode", true)->value();
    writeSettings();
    req->redirect("/");
  });

  // Slave registration – slave POSTs its IP here on connect
  server.on("/register", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("ip", true)) {
      slaveIP        = req->getParam("ip", true)->value();
      slaveConnected = true;
      Serial.println("Slave registered at IP: " + slaveIP);
    }
    req->send(200, "text/plain", "OK");
  });

  // Slave status endpoints – return cached values polled in main loop
  server.on("/slave/connected", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", slaveConnected ? "Connected" : "Not Connected");
  });
  server.on("/slave/powergood", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", slaveCachedPowergood);
  });
  server.on("/slave/voltage", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", slaveCachedVoltage);
  });
  server.on("/slave/position", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", slaveCachedPosition);
  });
  server.on("/slave/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", slaveCachedStatus);
  });

  // Slave motor control – queue commands, send to slave in main loop
  server.on("/slave/update", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("slider", true)) {
      slavePendingSpeed = req->getParam("slider", true)->value().toInt();
      slaveSpeedPending = true;
    }
    if (req->hasParam("positionControl", true)) {
      slavePendingPos = req->getParam("positionControl", true)->value().toInt();
      slavePosPending = true;
    }
    req->send(200);
  });

  server.on("/slave/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    slaveSettingsEnabled    = req->hasParam("enabled1", true)
                              ? "enabled" : "disabled";
    slaveSettingsVoltage    = req->hasParam("setvoltage", true)
                              ? req->getParam("setvoltage", true)->value() : "";
    slaveSettingsMicrosteps = req->hasParam("microsteps", true)
                              ? req->getParam("microsteps", true)->value() : "";
    slaveSettingsCurrent    = req->hasParam("current", true)
                              ? req->getParam("current", true)->value() : "";
    slaveSettingsStall      = req->hasParam("stall_threshold", true)
                              ? req->getParam("stall_threshold", true)->value() : "";
    slaveSettingsStandstill = req->hasParam("standstill_mode", true)
                              ? req->getParam("standstill_mode", true)->value() : "";
    slaveSettingsPending    = true;
    req->redirect("/");
  });

  server.begin();

  // Flash LED to signal setup complete
  digitalWrite(LED1, HIGH);
  delay(200);
  digitalWrite(LED1, LOW);
}

// ── Arduino loop ──────────────────────────────────────────────────────────────

/**
 * @brief Main execution loop – runs continuously after setup().
 *
 * Processes pending motor commands, forwards slave commands, polls slave
 * status, runs 100 Hz scheduled tasks, and handles position control stepping
 * and physical buttons.
 *
 * **Loop execution order**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * start
 * if (speedUpdatePending?) then (yes)
 *   :Apply master velocity\nto TMC2209;
 * endif
 * if (posUpdatePending?) then (yes)
 *   :Update master setPoint\n(±12800 or ±25600 microsteps);
 * endif
 * if (slaveSpeedPending?) then (yes)
 *   :slavePost("/update", "slider=…");
 * endif
 * if (slavePosPending?) then (yes)
 *   :slavePost("/update", "positionControl=…");
 * endif
 * if (slaveSettingsPending?) then (yes)
 *   :Build URL-encoded body\nslavePost("/save", body);
 * endif
 * if (slaveConnected &&\npoll interval elapsed?) then (yes)
 *   :pollSlave() – refresh\ncached status values;
 * endif
 * :100 Hz block\n(encoder, PG check,\nenable/disable TMC2209);
 * :Open-loop position stepping\n(STEP/DIR pulses);
 * :Button debounce\n& velocity update;
 * stop
 * @enduml
 *
 * @note The async web-server task sets `*Pending` flags from a separate
 *       FreeRTOS task.  All flags are declared `volatile` to prevent the
 *       compiler from caching them in registers.
 */
void loop() {

  // ── Apply master speed update ──
  if (speedUpdatePending) {
    set_speed = pendingSpeed;
    stepper_driver.moveAtVelocity(set_speed * microsteps.toInt());
    speedUpdatePending = false;
  }

  // ── Apply master position update ──
  if (posUpdatePending) {
    stepper_driver.moveAtVelocity(0);
    if      (pendingPosMode == 1) setPoint -= 25600;
    else if (pendingPosMode == 2) setPoint -= 12800;
    else if (pendingPosMode == 3) setPoint += 12800;
    else if (pendingPosMode == 4) setPoint += 25600;
    posUpdatePending = false;
  }

  // ── Forward pending slave speed command ──
  if (slaveSpeedPending) {
    slavePost("/update", "slider=" + String(slavePendingSpeed));
    slaveSpeedPending = false;
  }

  // ── Forward pending slave position command ──
  if (slavePosPending) {
    slavePost("/update", "positionControl=" + String(slavePendingPos));
    slavePosPending = false;
  }

  // ── Forward pending slave settings ──
  if (slaveSettingsPending) {
    String body = "";
    if (slaveSettingsEnabled == "enabled")    body += "enabled1=enabled&";
    if (!slaveSettingsVoltage.isEmpty())       body += "setvoltage="      + slaveSettingsVoltage    + "&";
    if (!slaveSettingsMicrosteps.isEmpty())    body += "microsteps="      + slaveSettingsMicrosteps + "&";
    if (!slaveSettingsCurrent.isEmpty())       body += "current="         + slaveSettingsCurrent    + "&";
    if (!slaveSettingsStall.isEmpty())         body += "stall_threshold=" + slaveSettingsStall      + "&";
    if (!slaveSettingsStandstill.isEmpty())    body += "standstill_mode=" + slaveSettingsStandstill;
    slavePost("/save", body);
    slaveSettingsPending = false;
  }

  // ── Poll slave status periodically ──
  if (slaveConnected && (millis() - lastSlavePoll >= slavePollInterval)) {
    lastSlavePoll = millis();
    pollSlave();
  }

  // ── Scheduled 100 Hz tasks ──
  if (millis() - lastEncRead >= (unsigned long)mainFreq) {
    lastEncRead = millis();

    digitalWrite(LED2, digitalRead(DIAG)); // Mirror stall flag on LED2

    PGState = digitalRead(PG);
    if (PGState == LOW && enabled1 == "enabled" && enabledState == 0) {
      stepper_driver.enable();
      enabledState = 1;
    } else if ((PGState == HIGH || enabled1 == "disabled") && enabledState == 1) {
      stepper_driver.disable();
      enabledState = 0;
    }
  }

  // ── Open-loop position control ──
  int microSteps         = microsteps.toInt();
  int delaySpeedAdjusted = 4500 / microSteps;

  if (setPoint > CurrentPosition) {
    if (micros() - lastStep > (unsigned long)delaySpeedAdjusted) {
      digitalWrite(DIR,  LOW);
      digitalWrite(STEP, state);
      state = !state;
      CurrentPosition += (256 / microSteps);
      lastStep = micros();
    }
  } else if (setPoint < CurrentPosition) {
    if (micros() - lastStep > (unsigned long)delaySpeedAdjusted) {
      digitalWrite(DIR,  HIGH);
      digitalWrite(STEP, state);
      state = !state;
      CurrentPosition -= (256 / microSteps);
      lastStep = micros();
    }
  }

  // ── Physical button control (separate velocity control) ──
  if ((millis() - lastDebounceTime) > debounceDelay) {
    lastDebounceTime = millis();
    bool curInc   = digitalRead(SW3);
    bool curDec   = digitalRead(SW1);
    bool curReset = digitalRead(SW2);

    if (curInc != incButtonState) {
      incButtonState = curInc;
      if (incButtonState == LOW) {
        buttonSpeed = min(buttonSpeed + 30, 330);
        stepper_driver.moveAtVelocity(buttonSpeed * microsteps.toInt());
      }
    }
    if (curDec != decButtonState) {
      decButtonState = curDec;
      if (decButtonState == LOW) {
        buttonSpeed = max(buttonSpeed - 30, -330);
        stepper_driver.moveAtVelocity(buttonSpeed * microsteps.toInt());
      }
    }
    if (curReset != resetButtonState) {
      resetButtonState = curReset;
      if (resetButtonState == LOW) {
        buttonSpeed = 0;
        stepper_driver.moveAtVelocity(0);
      }
    }
  }
}

// ── Encoder reading ───────────────────────────────────────────────────────────

/**
 * @brief Reads the AS5600 magnetic encoder over I2C and accumulates total
 *        rotation into #total_encoder_counts.
 *
 * The AS5600 reports a 12-bit raw angle (0–4095) that wraps at each full
 * rotation.  This function detects wrap-around crossings and maintains a
 * running revolution counter so that positions spanning multiple full turns
 * are represented correctly.
 *
 * **Wrap-around detection**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * start
 * :Read 2 bytes from AS5600 register 0x0C;
 * :raw_counts = (byte0 << 8) | byte1;
 * if (prev > 3000 AND raw < 1000?) then (yes)
 *   :revolutions++ (CCW wrap);
 * elseif (prev < 1000 AND raw > 3000?) then (yes)
 *   :revolutions-- (CW wrap);
 * endif
 * :prev_raw_counts = raw_counts;
 * :total_encoder_counts = raw + 4096 * revolutions;
 * stop
 * @enduml
 */
void readEncoder() {
  int raw_counts = 0;
  static int         prev_raw_counts = 0;
  static signed long revolutions     = 0;

  Wire.beginTransmission(AS5600_ADDRESS);
  Wire.write(0x0C); // Raw angle register
  Wire.endTransmission(false);
  Wire.requestFrom(AS5600_ADDRESS, 2);
  if (Wire.available() >= 2)
    raw_counts = Wire.read() << 8 | Wire.read();

  // Detect full-rotation wrap-around
  if      (prev_raw_counts > 3000 && raw_counts < 1000) revolutions++;
  else if (prev_raw_counts < 1000 && raw_counts > 3000) revolutions--;

  prev_raw_counts      = raw_counts;
  total_encoder_counts = raw_counts + (4096 * revolutions);
}

// ── Settings helpers ──────────────────────────────────────────────────────────

/**
 * @brief Applies the current global setting strings to the hardware peripherals.
 *
 * Sets the CH224K USB-PD voltage config pins and programs the TMC2209 via
 * UART (run current, microsteps, stall-guard threshold, standstill mode).
 * Should be called after any settings change.
 *
 * **USB-PD voltage truth table**
 * | setVoltage | CFG1 | CFG2 | CFG3 |
 * |-----------|------|------|------|
 * | "5"       |  H   |  -   |  -   |
 * | "9"       |  L   |  L   |  L   |
 * | "12"      |  L   |  L   |  H   |
 * | "15"      |  L   |  H   |  H   |
 * | "20"      |  L   |  H   |  L   |
 */
void configureSettings() {
  if      (setVoltage == "5")  { digitalWrite(CFG1, HIGH); }
  else if (setVoltage == "9")  { digitalWrite(CFG1, LOW); digitalWrite(CFG2, LOW);  digitalWrite(CFG3, LOW);  }
  else if (setVoltage == "12") { digitalWrite(CFG1, LOW); digitalWrite(CFG2, LOW);  digitalWrite(CFG3, HIGH); }
  else if (setVoltage == "15") { digitalWrite(CFG1, LOW); digitalWrite(CFG2, HIGH); digitalWrite(CFG3, HIGH); }
  else if (setVoltage == "20") { digitalWrite(CFG1, LOW); digitalWrite(CFG2, HIGH); digitalWrite(CFG3, LOW);  }

  stepper_driver.setRunCurrent(current.toInt());
  stepper_driver.setMicrostepsPerStep(microsteps.toInt());
  stepper_driver.setStallGuardThreshold(stallThreshold.toInt());

  if      (standstillMode == "NORMAL")         stepper_driver.setStandstillMode(stepper_driver.NORMAL);
  else if (standstillMode == "FREEWHEELING")   stepper_driver.setStandstillMode(stepper_driver.FREEWHEELING);
  else if (standstillMode == "BRAKING")        stepper_driver.setStandstillMode(stepper_driver.BRAKING);
  else if (standstillMode == "STRONG_BRAKING") stepper_driver.setStandstillMode(stepper_driver.STRONG_BRAKING);
}

/**
 * @brief Loads persisted settings from NVS flash into global variables.
 *
 * Uses the ESP32 Preferences library under namespace `"settings"`.
 * On the very first boot (no key present) default values are written via
 * writeSettings().
 */
void readSettings() {
  preferences.begin("settings", false);
  enabled1 = preferences.getString("enable", "");
  if (enabled1.isEmpty()) { // First boot – write defaults
    preferences.end();
    enabled1       = "enabled";
    setVoltage     = "12";
    microsteps     = "32";
    current        = "30";
    stallThreshold = "10";
    standstillMode = "NORMAL";
    writeSettings();
  } else {
    setVoltage     = preferences.getString("voltage",        "12");
    microsteps     = preferences.getString("microsteps",     "32");
    current        = preferences.getString("current",        "30");
    stallThreshold = preferences.getString("stallThreshold", "10");
    standstillMode = preferences.getString("standstillMode", "NORMAL");
    preferences.end();
  }
}

/**
 * @brief Persists current global settings to NVS flash and applies them.
 *
 * Writes all setting strings to the `"settings"` Preferences namespace,
 * then calls configureSettings() to push the new values to hardware.
 */
void writeSettings() {
  preferences.begin("settings", false);
  preferences.putString("enable",         enabled1);
  preferences.putString("voltage",        setVoltage);
  preferences.putString("microsteps",     microsteps);
  preferences.putString("current",        current);
  preferences.putString("stallThreshold", stallThreshold);
  preferences.putString("standstillMode", standstillMode);
  preferences.end();
  configureSettings();
}
