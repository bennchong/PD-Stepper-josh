# PD Stepper – Host-Side Testing Guide

Two complementary layers of testing are provided for the Master/Slave
firmware.

---

## 1. Cross-compilation check (CI – no hardware required)

The GitHub Actions workflow `.github/workflows/arduino-compile.yml` uses
[`arduino/compile-sketches`](https://github.com/arduino/compile-sketches)
to compile both sketches for the ESP32-S3 target.  It installs the Espressif
board package and all required libraries automatically.

**What it catches:** syntax errors, type mismatches, missing includes,
undeclared identifiers.

The workflow runs automatically on every push/PR that touches
`Software/Master_Slave_Web_Server/`.

### Run locally with arduino-cli

```bash
# Install arduino-cli (Linux)
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
export PATH=$PATH:$HOME/bin

# Add ESP32 board support
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install required libraries
arduino-cli lib install "TMC2209"
arduino-cli lib install --git-url \
  https://github.com/ESP32Async/ESPAsyncWebServer.git
arduino-cli lib install --git-url \
  https://github.com/ESP32Async/AsyncTCP.git

# Compile both sketches
arduino-cli compile --fqbn esp32:esp32:esp32s3 \
  Software/Master_Slave_Web_Server/Master/Master.ino

arduino-cli compile --fqbn esp32:esp32:esp32s3 \
  Software/Master_Slave_Web_Server/Slave/Slave.ino
```

---

## 2. Native host unit tests (Linux – no hardware required)

The `test/` directory contains a Google Test suite that compiles the `.ino`
files as plain C++ on your Linux workstation, substituting thin mock
implementations for all Arduino/ESP32 hardware APIs.

### Prerequisites

```bash
sudo apt-get install build-essential libgtest-dev
```

### Build and run

```bash
cd Software/Master_Slave_Web_Server/test
make test
```

Expected output:

```
══════════════════════════════════════════
  Running Master tests
══════════════════════════════════════════
[==========] Running 30 tests from 7 test suites.
[----------] ...
[  PASSED  ] 30 tests.

══════════════════════════════════════════
  Running Slave tests
══════════════════════════════════════════
[==========] Running 17 tests from 6 test suites.
[----------] ...
[  PASSED  ] 17 tests.
```

### What is tested

| Test suite (Master)   | What it verifies |
|-----------------------|-----------------|
| `ProcessorTest`       | HTML template variable substitution (`processor()`) |
| `VoltageTest`         | ADC averaging + resistor-divider formula |
| `EncoderTest`         | AS5600 wrap-around detection (CCW/CW, multi-turn) |
| `CfgPinTest`          | USB-PD CFG1/2/3 truth table for each voltage |
| `PollSlaveTest`       | Slave disconnect detection when responses are N/A |
| `PositionStepTest`    | setPoint increment/decrement for modes 1–4 |
| `SettingsTest`        | NVS read/write round-trip and first-boot defaults |

| Test suite (Slave)    | What it verifies |
|-----------------------|-----------------|
| `SlaveVoltage`        | Same ADC formula |
| `SlaveEncoderTest`    | Same wrap-around logic |
| `SlaveCfgPinTest`     | Same USB-PD CFG truth table |
| `RegisterTest`        | `registerWithMaster()` guards, HTTP 200 vs non-200 |
| `SlaveSettingsTest`   | Pending settings applied in `loop()` + persisted to flash |
| `SlavePositionTest`   | setPoint modes 1–4 |

### Mock architecture

```
test/
├── Makefile
├── README.md
├── mocks/
│   ├── Arduino.h          # GPIO (gpio_out[]/gpio_in[]), millis/micros, Serial
│   ├── WString.h          # Arduino String class backed by std::string
│   ├── Wire.h             # TwoWire – setResponse() injects I2C read data
│   ├── WiFi.h             # WiFiClass – _status field controls WL_CONNECTED
│   ├── Preferences.h      # In-memory NVS using std::map
│   ├── HTTPClient.h       # _mock_code / _mock_body static fields
│   ├── TMC2209.h          # _velocity/_enabled/_stall_guard inspectable fields
│   ├── ESPAsyncWebServer.h# setParam() injects POST/GET parameters
│   └── AsyncTCP.h         # empty stub (internal to ESPAsyncWebServer)
├── test_master.cpp        # Google Test cases for Master.ino
└── test_slave.cpp         # Google Test cases for Slave.ino
```

The `.ino` files are `#include`d directly into each test binary so the
actual firmware code is compiled and tested, not a re-implementation.
