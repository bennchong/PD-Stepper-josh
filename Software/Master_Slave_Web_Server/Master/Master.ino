/*
 * PD Stepper Master/Slave Web Server - MASTER
 *
 *  Software Version 1.0
 *
 *  How to Use:
 * 1. Flash this sketch to the MASTER PD Stepper board.
 * 2. Flash the Slave sketch to the SLAVE PD Stepper board.
 * 3. Power on both boards; the slave will auto-connect to the master's WiFi AP
 *    and register itself.
 * 4. On a browser, connect to WiFi "PD Stepper Master" then visit 192.168.4.1
 *    to control both motors from a single page.
 *
 *  Architecture:
 *   Browser  →  Master HTTP server (192.168.4.1)
 *   Master   →  Slave HTTP server  (slave's DHCP IP) via HTTPClient in main loop
 *
 *  For more info and to purchase PD Stepper kits visit:
 *  https://thingsbyjosh.com
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>  // https://github.com/ESP32Async/ESPAsyncWebServer
#include <AsyncTCP.h>           // https://github.com/ESP32Async/AsyncTCP
#include <TMC2209.h>            // https://github.com/janelia-arduino/TMC2209
#include <Preferences.h>
#include <HTTPClient.h>
#include "master_index_html.h"

Preferences preferences;

// WiFi Access Point credentials
const char *ssid     = "PD Stepper Master";
const char *password = "";

AsyncWebServer server(80);

// TMC2209 stepper driver
TMC2209 stepper_driver;
HardwareSerial &serial_stream     = Serial2;
const long      SERIAL_BAUD_RATE  = 115200;
const uint8_t   RUN_CURRENT_PERCENT = 100;

// ── Pin definitions ───────────────────────────────────────────────────────────
// TMC2209 stepper driver
#define TMC_EN  21
#define STEP     5
#define DIR      6
#define MS1      1
#define MS2      2
#define SPREAD   7
#define TMC_TX  17
#define TMC_RX  18
#define DIAG    16
#define INDEX   11

// USB-PD trigger (CH224K)
#define PG   15
#define CFG1 38
#define CFG2 48
#define CFG3 47

// Misc
#define VBUS  4
#define NTC   7
#define LED1 10
#define LED2 12
#define SW1  35
#define SW2  36
#define SW3  37
#define AUX1 14
#define AUX2 13

// AS5600 Hall-effect encoder (I2C)
#include <Wire.h>
#define AS5600_ADDRESS 0x36
signed long   total_encoder_counts = 0;
unsigned long lastEncRead           = 0;
int           mainFreq              = 10; // 100 Hz scheduled tasks

// ── Global state ──────────────────────────────────────────────────────────────
int  set_speed    = 0;
bool PGState      = 0;
bool enabledState = 0;
bool state        = 0;  // step toggle

// Button debounce
bool incButtonState   = HIGH;
bool decButtonState   = HIGH;
bool resetButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;
int buttonSpeed = 0;

// Voltage reading
float VBusVoltage = 0;
const float VREF      = 3.3;
const float DIV_RATIO = 0.1189427313; // 20 kΩ / 2.7 kΩ divider

// ── Settings persisted to flash ───────────────────────────────────────────────
String enabled1       = "enabled";
String setVoltage     = "12";
String microsteps     = "32";
String current        = "30";
String stallThreshold = "10";
String standstillMode = "NORMAL";

// ── Pending motor-update flags (set in async callbacks, consumed in loop) ─────
volatile bool speedUpdatePending = false;
volatile int  pendingSpeed       = 0;
volatile bool posUpdatePending   = false;
volatile int  pendingPosMode     = 0;

// Open-loop position control
signed long   setPoint        = 0;
signed long   CurrentPosition = 0;
unsigned long lastStep        = 0;

// ── Slave management ──────────────────────────────────────────────────────────
String slaveIP        = "";
bool   slaveConnected = false;

unsigned long lastSlavePoll = 0;
const unsigned long slavePollInterval = 2000; // ms between status polls

// Cached slave status (refreshed by pollSlave() in main loop)
String slaveCachedVoltage   = "N/A";
String slaveCachedPosition  = "N/A";
String slaveCachedStatus    = "Not Connected";
String slaveCachedPowergood = "N/A";

// Pending slave motor commands
volatile bool slaveSpeedPending = false;
volatile int  slavePendingSpeed = 0;
volatile bool slavePosPending   = false;
volatile int  slavePendingPos   = 0;

// Pending slave settings (built from /slave/save POST, sent in loop)
volatile bool slaveSettingsPending = false;
String slaveSettingsEnabled    = "";
String slaveSettingsVoltage    = "";
String slaveSettingsMicrosteps = "";
String slaveSettingsCurrent    = "";
String slaveSettingsStall      = "";
String slaveSettingsStandstill = "";

// ── Forward declarations ──────────────────────────────────────────────────────
void readEncoder();
void configureSettings();
void readSettings();
void writeSettings();

// ── Status-reading helpers ────────────────────────────────────────────────────

String readPGState() {
  PGState = digitalRead(PG);
  return (PGState == 0) ? "Power Good" : "Power Bad";
}

String readVoltage() {
  uint32_t mvSum = 0;
  for (int i = 0; i < 10; i++) mvSum += analogReadMilliVolts(VBUS);
  VBusVoltage = ((float)mvSum / 10.0 / 1000.0) / DIV_RATIO;
  return String(VBusVoltage, 2) + "V";
}

String readEncoderPos() {
  readEncoder();
  return String(total_encoder_counts);
}

String readTMCStatus() {
  if (stepper_driver.hardwareDisabled()) return "Hardware Disabled";
  TMC2209::Status s = stepper_driver.getStatus();
  if (s.over_temperature_warning)  return "Over Temp Warning";
  if (s.over_temperature_shutdown) return "Over Temp Shutdown";
  return "No Errors";
}

String readStallStatus() {
  return String(stepper_driver.getStallGuardResult());
}

// Template processor – substitutes %VAR% in master_index_html
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

// Refresh all cached slave status values (called from main loop periodically)
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
