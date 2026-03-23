/**
 * @file Slave.ino
 * @brief PD Stepper Slave firmware – connects to the master board's WiFi AP,
 *        registers itself, and accepts motor-control commands forwarded by the
 *        master.
 * @version 1.0
 *
 * @details
 * The slave board performs the following roles:
 *  -# Connects to the master's WiFi AP (`"PD Stepper Master"`) as a station.
 *  -# Registers its DHCP IP address with the master via `POST /register`.
 *  -# Runs an HTTP server that the master queries for status and to which it
 *     forwards velocity/settings commands received from the browser.
 *  -# Controls its own TMC2209 stepper identically to the single-motor sketch.
 *
 * **System context**
 * @startuml
 * !theme plain
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * skinparam componentBorderColor #555
 *
 * actor Browser
 * component "Master\nESP32-S3\n192.168.4.1" as Master #fc4903
 * component "Slave\nESP32-S3\n192.168.4.x" as Slave #4f9cf8
 *
 * Browser --> Master : HTTP GET/POST
 * Master  --> Slave  : GET /voltage, /position, /status …\nPOST /update, /save
 * Slave   --> Master : POST /register (on boot / reconnect)
 * @enduml
 *
 * **LED1 status indicator**
 * | State       | Meaning                          |
 * |-------------|----------------------------------|
 * | Blinking    | Connecting to master AP          |
 * | Solid ON    | Connected and registered         |
 * | OFF         | WiFi disconnected                |
 *
 * **How to use**
 * 1. Flash this sketch to the **SLAVE** PD Stepper board.
 * 2. Flash Master.ino to the **MASTER** PD Stepper board.
 * 3. Power on both boards; the slave connects and registers automatically.
 * 4. Control both motors from the master's web UI at `192.168.4.1`.
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

Preferences preferences;

/** @brief SSID of the master board's WiFi AP – must match Master.ino. */
const char *masterSSID     = "PD Stepper Master";
/** @brief Password for the master AP (empty = open network). */
const char *masterPassword = "";
/** @brief Fixed IP of the master board (default AP gateway). */
const char *masterIP       = "192.168.4.1";

/** @brief Async HTTP server listening on port 80. */
AsyncWebServer server(80);

// TMC2209 stepper driver
TMC2209 stepper_driver;
HardwareSerial &serial_stream     = Serial2;
const long      SERIAL_BAUD_RATE  = 115200;
const uint8_t   RUN_CURRENT_PERCENT = 100;

/**
 * @defgroup SlavePins Pin definitions (ESP32-S3 GPIO)
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
#define LED1 10  ///< Status LED 1 (WiFi / registration indicator)
#define LED2 12  ///< Status LED 2 (mirrors DIAG/stall)
#define SW1  35  ///< Button 1 – decrease speed (local fallback)
#define SW2  36  ///< Button 2 – reset / stop   (local fallback)
#define SW3  37  ///< Button 3 – increase speed (local fallback)
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
unsigned long lastDebounceTime = 0;        ///< Last debounce timestamp (ms)
const unsigned long debounceDelay = 50;    ///< Debounce window in ms
int buttonSpeed = 0;                       ///< Current button-driven velocity

// Voltage reading
float VBusVoltage = 0;                       ///< Last computed VBUS voltage (V)
const float VREF      = 3.3;                 ///< ESP32 ADC reference voltage
const float DIV_RATIO = 0.1189427313;        ///< 20 kΩ / 2.7 kΩ divider ratio

/**
 * @defgroup SlaveSettings Persistent settings (stored in NVS flash)
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
 * @defgroup SlavePending Async-to-loop pending flags
 * ESPAsyncWebServer callbacks run on a FreeRTOS task separate from loop().
 * Commands are staged here and consumed safely inside loop().
 * @{
 */
volatile bool speedUpdatePending  = false;  ///< New velocity command from master
volatile int  pendingSpeed        = 0;      ///< Velocity to apply
volatile bool posUpdatePending    = false;  ///< New position step from master
volatile int  pendingPosMode      = 0;      ///< 1=--large, 2=-small, 3=+small, 4=++large
volatile bool saveSettingsPending = false;  ///< New settings from master

// Pending settings values (set in async callback, consumed in loop)
String pendingEnabled    = "";  ///< Buffered enable state
String pendingVoltage    = "";  ///< Buffered voltage setting
String pendingMicrosteps = "";  ///< Buffered microstep setting
String pendingCurrent    = "";  ///< Buffered current setting
String pendingStall      = "";  ///< Buffered stall threshold
String pendingStandstill = "";  ///< Buffered standstill mode
/** @} */

// Open-loop position control
signed long   setPoint        = 0;  ///< Desired position in microstep counts
signed long   CurrentPosition = 0;  ///< Current tracked position in microstep counts
unsigned long lastStep        = 0;  ///< Timestamp of last STEP pulse (µs)

/**
 * @defgroup SlaveWiFi WiFi reconnect management
 * @{
 */
/** @brief True once the slave has successfully POSTed its IP to the master. */
bool          registered          = false;
/** @brief Timestamp of last WiFi / registration health check (ms). */
unsigned long lastWiFiCheck       = 0;
/** @brief Interval between WiFi / registration health checks (ms). */
const unsigned long wifiCheckInterval = 5000;
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

// ── Master registration ───────────────────────────────────────────────────────

/**
 * @brief POSTs the slave's local IP address to the master's `/register` endpoint.
 *
 * Sets #registered to `true` on success so that loop() knows not to retry
 * until WiFi drops again.  Returns immediately if WiFi is not connected.
 *
 * **Registration sequence**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * participant "Slave\nregisterWithMaster()" as S
 * participant "Master\nHTTP server" as M
 *
 * S  -> S  : WiFi.status() == WL_CONNECTED?
 * alt not connected
 *   S  --> S : return (no-op)
 * else connected
 *   S  -> S  : myIP = WiFi.localIP().toString()
 *   S  -> M  : POST /register\nip=192.168.4.xxx
 *   M  -> M  : slaveIP = ip\nslaveConnected = true
 *   M  --> S : HTTP 200 OK
 *   S  -> S  : registered = true\nSerial.println(…)
 * end
 * @enduml
 */
void registerWithMaster() {
  if (WiFi.status() != WL_CONNECTED) return;
  String myIP = WiFi.localIP().toString();
  HTTPClient http;
  http.begin("http://" + String(masterIP) + "/register");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(3000);
  int code = http.POST("ip=" + myIP);
  http.end();
  if (code == 200) {
    registered = true;
    Serial.println("Registered with master as " + myIP);
  } else {
    Serial.println("Registration failed (HTTP " + String(code) + ")");
  }
}

// ── Arduino setup ─────────────────────────────────────────────────────────────

/**
 * @brief One-time initialisation: GPIO, TMC2209, WiFi STA, HTTP routes, and
 *        initial registration with the master.
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
 * :WiFi.mode(WIFI_STA)\nWiFi.begin("PD Stepper Master");
 * :Wait up to 15 s for WL_CONNECTED\n(blink LED1 while waiting);
 * if (WL_CONNECTED?) then (yes)
 *   :LED1 = HIGH (solid);
 * else (no)
 *   :LED1 = LOW\nWill retry in loop();
 * endif
 * :Register all HTTP routes;
 * :server.begin();
 * :registerWithMaster();
 * :Triple-flash LED1 (setup complete);
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
  Serial.println("Slave starting...");

  // Connect to master's WiFi Access Point
  WiFi.mode(WIFI_STA);
  WiFi.begin(masterSSID, masterPassword);
  Serial.print("Connecting to master WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED1, !digitalRead(LED1)); // Blink while connecting
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED1, HIGH); // Solid on = connected
  } else {
    Serial.println("Connection timeout – will retry in loop");
    digitalWrite(LED1, LOW);
  }

  // ── Web-server routes ────────────────────────────────────────────────────

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

  // Motor velocity / position control (commands forwarded by master)
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

  // Settings update (forwarded by master; reply 200 – no browser redirect)
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    pendingEnabled    = req->hasParam("enabled1", true)
                        ? "enabled" : "disabled";
    pendingVoltage    = req->hasParam("setvoltage", true)
                        ? req->getParam("setvoltage", true)->value() : setVoltage;
    pendingMicrosteps = req->hasParam("microsteps", true)
                        ? req->getParam("microsteps", true)->value() : microsteps;
    pendingCurrent    = req->hasParam("current", true)
                        ? req->getParam("current", true)->value() : current;
    pendingStall      = req->hasParam("stall_threshold", true)
                        ? req->getParam("stall_threshold", true)->value() : stallThreshold;
    pendingStandstill = req->hasParam("standstill_mode", true)
                        ? req->getParam("standstill_mode", true)->value() : standstillMode;
    saveSettingsPending = true;
    req->send(200, "text/plain", "Saved");
  });

  server.begin();

  // Register IP with master
  registerWithMaster();

  // Flash LED to signal setup complete
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED1, HIGH);
    delay(100);
    digitalWrite(LED1, LOW);
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) digitalWrite(LED1, HIGH);
}

// ── Arduino loop ──────────────────────────────────────────────────────────────

/**
 * @brief Main execution loop – runs continuously after setup().
 *
 * Handles WiFi/registration health checks, applies queued motor commands,
 * runs 100 Hz scheduled tasks, and handles position control stepping and
 * physical button fallback.
 *
 * **WiFi reconnect state machine**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 *
 * [*]            --> Connecting    : power on / WiFi.begin()
 * Connecting     --> Connected     : WL_CONNECTED within 15 s
 * Connecting     --> Disconnected  : timeout (will retry in loop)
 * Connected      --> Registering   : registerWithMaster() called
 * Registering    --> Operational   : HTTP 200 OK from master
 * Registering    --> Registering   : non-200 → retry next interval
 * Operational    --> Disconnected  : WiFi.status() != WL_CONNECTED
 * Disconnected   --> Connecting    : WiFi.reconnect()
 * Connecting     --> Registering   : WL_CONNECTED (re-connect)
 *
 * note right of Operational
 *   LED1 = HIGH
 *   Motor responds to
 *   /update and /save
 * end note
 * note right of Disconnected
 *   LED1 = OFF
 *   registered = false
 * end note
 * @enduml
 *
 * **Loop execution order**
 * @startuml
 * skinparam backgroundColor #232323
 * skinparam defaultFontColor #efefef
 * start
 * if (wifiCheckInterval elapsed?) then (yes)
 *   if (WiFi disconnected?) then (yes)
 *     :registered=false\nLED1=LOW\nWiFi.reconnect();
 *   elseif (not registered?) then (yes)
 *     :registerWithMaster();
 *   endif
 * endif
 * if (speedUpdatePending?) then (yes)
 *   :Apply velocity to TMC2209;
 * endif
 * if (posUpdatePending?) then (yes)
 *   :Update setPoint;
 * endif
 * if (saveSettingsPending?) then (yes)
 *   :Apply and persist\nnew settings;
 * endif
 * :100 Hz block\n(encoder, PG, enable/disable);
 * :Open-loop position stepping;
 * :Button debounce & local speed;
 * stop
 * @enduml
 */
void loop() {

  // ── WiFi reconnection and re-registration ──
  if (millis() - lastWiFiCheck > wifiCheckInterval) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      registered = false;
      digitalWrite(LED1, LOW);
      WiFi.reconnect();
    } else if (!registered) {
      registerWithMaster();
      if (registered) digitalWrite(LED1, HIGH);
    }
  }

  // ── Apply speed update ──
  if (speedUpdatePending) {
    set_speed = pendingSpeed;
    stepper_driver.moveAtVelocity(set_speed * microsteps.toInt());
    speedUpdatePending = false;
  }

  // ── Apply position update ──
  if (posUpdatePending) {
    stepper_driver.moveAtVelocity(0);
    if      (pendingPosMode == 1) setPoint -= 25600;
    else if (pendingPosMode == 2) setPoint -= 12800;
    else if (pendingPosMode == 3) setPoint += 12800;
    else if (pendingPosMode == 4) setPoint += 25600;
    posUpdatePending = false;
  }

  // ── Apply saved settings ──
  if (saveSettingsPending) {
    enabled1 = pendingEnabled;
    if (pendingVoltage != setVoltage) {
      setVoltage = pendingVoltage;
      stepper_driver.moveAtVelocity(0);
    }
    if (pendingMicrosteps != microsteps) {
      microsteps = pendingMicrosteps;
      stepper_driver.moveAtVelocity(0);
    }
    current        = pendingCurrent;
    stallThreshold = pendingStall;
    standstillMode = pendingStandstill;
    writeSettings();
    saveSettingsPending = false;
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

  // ── Physical button control (local fallback) ──
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
