/**
 * @file test_master.cpp
 * @brief Native host unit tests for Master.ino business logic.
 *
 * Compiles Master.ino on a Linux host (via Arduino API mocks) and exercises
 * the pure-logic functions with Google Test.
 *
 * Build & run:
 * @code
 *   cd Software/Master_Slave_Web_Server/test
 *   make
 *   ./test_master
 * @endcode
 */

#include <gtest/gtest.h>
#include <map>

// ── Instantiate mock global state ─────────────────────────────────────────
// These symbols are declared 'extern' in the mock headers; we define them
// here so the linker can resolve them.
int           gpio_out[256]  = {};
int           gpio_in[256]   = {};
uint32_t      analog_mv[256] = {};
unsigned long _mock_millis   = 0;
unsigned long _mock_micros   = 0;

// ── Pull in mock headers BEFORE the .ino so its #includes are intercepted ─
#include "mocks/Arduino.h"
#include "mocks/Wire.h"
#include "mocks/WiFi.h"
#include "mocks/Preferences.h"
#include "mocks/HTTPClient.h"
#include "mocks/TMC2209.h"
#include "mocks/AsyncTCP.h"
#include "mocks/ESPAsyncWebServer.h"

// ── Define objects declared extern in the mock headers ────────────────────
HardwareSerial Serial;
HardwareSerial Serial2;
TwoWire        Wire;
WiFiClass      WiFi;
int    HTTPClient::_mock_code = 200;
String HTTPClient::_mock_body = "OK";

// ── Include Master.ino as a C++ translation unit ──────────────────────────
// setup() and loop() are defined but not called; only the helpers are tested.
#include "../Master/Master.ino"

// =========================================================================
// processor() – HTML template variable substitution
// =========================================================================

class ProcessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset to known defaults before each test
        enabled1       = "enabled";
        microsteps     = "32";
        setVoltage     = "12";
        current        = "30";
        stallThreshold = "10";
        standstillMode = "NORMAL";
    }
};

TEST_F(ProcessorTest, Enabled1ReturnCheckedWhenEnabled) {
    enabled1 = "enabled";
    EXPECT_EQ(processor("enabled1"), String("checked"));
}

TEST_F(ProcessorTest, Enabled1ReturnsEmptyWhenDisabled) {
    enabled1 = "disabled";
    EXPECT_EQ(processor("enabled1"), String(""));
}

TEST_F(ProcessorTest, MicrostepsMapped) {
    microsteps = "16";
    EXPECT_EQ(processor("microsteps"), String("16"));
}

TEST_F(ProcessorTest, VoltageMapped) {
    setVoltage = "20";
    EXPECT_EQ(processor("voltage"), String("20"));
}

TEST_F(ProcessorTest, CurrentMapped) {
    current = "75";
    EXPECT_EQ(processor("current"), String("75"));
}

TEST_F(ProcessorTest, StallThresholdMapped) {
    stallThreshold = "42";
    EXPECT_EQ(processor("stall_threshold"), String("42"));
}

TEST_F(ProcessorTest, StandstillModeMapped) {
    standstillMode = "FREEWHEELING";
    EXPECT_EQ(processor("standstill_mode"), String("FREEWHEELING"));
}

TEST_F(ProcessorTest, UnknownVarReturnsEmpty) {
    EXPECT_EQ(processor("unknown_var"), String(""));
    EXPECT_EQ(processor(""),            String(""));
}

// =========================================================================
// readVoltage() – ADC averaging + resistor-divider maths
// =========================================================================

class VoltageTest : public ::testing::Test {
protected:
    void SetUp() override { _mock_millis = 0; }
};

TEST_F(VoltageTest, TwelveVoltReadingIsCorrect) {
    // 12 V * DIV_RATIO(0.1189427313) = 1427.31 mV.  All 10 samples identical.
    for (int i = 0; i < 256; i++) analog_mv[i] = 1427;
    String result = readVoltage();
    // Expect something close to "12.00V" (allow rounding in last digit)
    EXPECT_EQ(result.c_str()[result.length()-1], 'V');
    float v = atof(result.c_str());
    EXPECT_NEAR(v, 12.0f, 0.15f);
}

TEST_F(VoltageTest, FiveVoltReadingIsCorrect) {
    // 5 V * DIV_RATIO = 594.71 mV
    for (int i = 0; i < 256; i++) analog_mv[i] = 595;
    String result = readVoltage();
    float v = atof(result.c_str());
    EXPECT_NEAR(v, 5.0f, 0.15f);
}

TEST_F(VoltageTest, ResultEndsWithV) {
    for (int i = 0; i < 256; i++) analog_mv[i] = 1000;
    String result = readVoltage();
    EXPECT_EQ(result.c_str()[result.length()-1], 'V');
}

// =========================================================================
// readEncoder() – AS5600 full-rotation wrap-around detection
//
// readEncoder() uses static-local variables (prev_raw_counts, revolutions)
// that persist across calls within the same binary run.  All assertions
// here use *deltas* so they remain correct regardless of accumulated state.
// =========================================================================

class EncoderTest : public ::testing::Test {
protected:
    /// Helper: set Wire mock bytes and call readEncoder()
    void readWith(int raw) {
        Wire.setResponse((uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF));
        readEncoder();
    }
    /// Helper: capture the current count, advance, return the delta
    long deltaAfter(int raw) {
        long before = total_encoder_counts;
        readWith(raw);
        return total_encoder_counts - before;
    }
};

TEST_F(EncoderTest, SameValueGivesZeroDelta) {
    readWith(2048);
    EXPECT_EQ(deltaAfter(2048), 0);
}

TEST_F(EncoderTest, CCWWrapProducesCorrectDelta) {
    // Establish prev near the top of range
    readWith(3500);
    // CCW crossing: prev > 3000, raw < 1000 → revolutions++
    // delta = raw - prev + 4096 = 100 - 3500 + 4096 = 696
    EXPECT_EQ(deltaAfter(100), 100 - 3500 + 4096);
}

TEST_F(EncoderTest, CWWrapProducesCorrectDelta) {
    // Establish prev near the bottom of range
    readWith(100);
    // CW crossing: prev < 1000, raw > 3000 → revolutions--
    // delta = raw - prev - 4096 = 3500 - 100 - 4096 = -696
    EXPECT_EQ(deltaAfter(3500), 3500 - 100 - 4096);
}

TEST_F(EncoderTest, TwoConsecutiveCCWWraps) {
    // Sequence: 3500 → 100 (wrap) → 2000 (no wrap) → 3500 (no wrap) → 100 (wrap)
    readWith(3500);
    long start = total_encoder_counts;
    readWith(100);   // CCW wrap: +696
    readWith(2000);  // no wrap: +1900
    readWith(3500);  // no wrap: +1500
    readWith(100);   // CCW wrap: +696
    long end = total_encoder_counts;
    EXPECT_EQ(end - start, 696 + 1900 + 1500 + 696);
}

TEST_F(EncoderTest, MidRangeMovementNoWrap) {
    readWith(2000);
    EXPECT_EQ(deltaAfter(2100), 100);
}

// =========================================================================
// configureSettings() – USB-PD CFG pin truth table
// =========================================================================

class CfgPinTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(gpio_out, 0, sizeof(gpio_out));
        // Defaults that satisfy stepper_driver mock calls
        current        = "30";
        microsteps     = "32";
        stallThreshold = "10";
        standstillMode = "NORMAL";
    }
    void configure(const char *v) { setVoltage = v; configureSettings(); }
};

TEST_F(CfgPinTest, FiveVolts_CFG1High) {
    configure("5");
    EXPECT_EQ(gpio_out[CFG1], HIGH);
}

TEST_F(CfgPinTest, NineVolts_AllCfgLow) {
    configure("9");
    EXPECT_EQ(gpio_out[CFG1], LOW);
    EXPECT_EQ(gpio_out[CFG2], LOW);
    EXPECT_EQ(gpio_out[CFG3], LOW);
}

TEST_F(CfgPinTest, TwelveVolts_CFG3High) {
    configure("12");
    EXPECT_EQ(gpio_out[CFG1], LOW);
    EXPECT_EQ(gpio_out[CFG2], LOW);
    EXPECT_EQ(gpio_out[CFG3], HIGH);
}

TEST_F(CfgPinTest, FifteenVolts_CFG2andCFG3High) {
    configure("15");
    EXPECT_EQ(gpio_out[CFG1], LOW);
    EXPECT_EQ(gpio_out[CFG2], HIGH);
    EXPECT_EQ(gpio_out[CFG3], HIGH);
}

TEST_F(CfgPinTest, TwentyVolts_CFG2High_CFG3Low) {
    configure("20");
    EXPECT_EQ(gpio_out[CFG1], LOW);
    EXPECT_EQ(gpio_out[CFG2], HIGH);
    EXPECT_EQ(gpio_out[CFG3], LOW);
}

// =========================================================================
// pollSlave() – disconnect detection when all responses are "N/A"
// =========================================================================

class PollSlaveTest : public ::testing::Test {
protected:
    void SetUp() override {
        slaveConnected      = true;
        slaveIP             = "192.168.4.100";
        slaveCachedVoltage  = "12.00V";
        slaveCachedStatus   = "No Errors";
        HTTPClient::_mock_code = 200;
        HTTPClient::_mock_body = "12.00V";
    }
};

TEST_F(PollSlaveTest, MarksDisconnectedWhenAllResponsesNA) {
    HTTPClient::_mock_code = 500; // Will produce "N/A" in slaveGet()
    pollSlave();
    EXPECT_FALSE(slaveConnected);
    EXPECT_EQ(slaveCachedStatus, String("Not Connected"));
}

TEST_F(PollSlaveTest, RemainsConnectedOnSuccess) {
    HTTPClient::_mock_code = 200;
    HTTPClient::_mock_body = "12.00V";
    pollSlave();
    EXPECT_TRUE(slaveConnected);
}

// =========================================================================
// loop() – master position step modes update setPoint correctly
// =========================================================================

class PositionStepTest : public ::testing::Test {
protected:
    void SetUp() override {
        setPoint       = 0;
        CurrentPosition = 0;
        microsteps     = "32";
        enabled1       = "disabled";  // keep driver quiet
        speedUpdatePending = false;
        posUpdatePending   = false;
        slaveSpeedPending  = false;
        slavePosPending    = false;
        slaveSettingsPending = false;
        slaveConnected     = false;
        _mock_millis = 10000;  // past all timer thresholds
        lastEncRead  = 0;
        lastSlavePoll = 0;
        lastDebounceTime = 0;
    }
    void applyMode(int mode) {
        pendingPosMode   = mode;
        posUpdatePending = true;
        loop();
    }
};

TEST_F(PositionStepTest, Mode1DecrementsByLargeStep) {
    applyMode(1);
    EXPECT_EQ(setPoint, -25600L);
}

TEST_F(PositionStepTest, Mode2DecrementsBySmallStep) {
    applyMode(2);
    EXPECT_EQ(setPoint, -12800L);
}

TEST_F(PositionStepTest, Mode3IncrementsBySmallStep) {
    applyMode(3);
    EXPECT_EQ(setPoint, 12800L);
}

TEST_F(PositionStepTest, Mode4IncrementsByLargeStep) {
    applyMode(4);
    EXPECT_EQ(setPoint, 25600L);
}

TEST_F(PositionStepTest, PositionIsAdditive) {
    applyMode(4);  // +25600
    applyMode(3);  // +12800
    EXPECT_EQ(setPoint, 38400L);
}

// =========================================================================
// settings persistence – readSettings/writeSettings round-trip
// =========================================================================

class SettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        preferences._store.clear();
        enabled1       = "enabled";
        setVoltage     = "12";
        microsteps     = "32";
        current        = "30";
        stallThreshold = "10";
        standstillMode = "NORMAL";
        memset(gpio_out, 0, sizeof(gpio_out));
    }
};

TEST_F(SettingsTest, WriteAndReadRoundTrip) {
    setVoltage = "20";
    current    = "75";
    writeSettings();

    // Corrupt in-memory values to prove readSettings() restores them
    setVoltage = "0";
    current    = "0";

    readSettings();
    EXPECT_EQ(setVoltage, String("20"));
    EXPECT_EQ(current,    String("75"));
}

TEST_F(SettingsTest, FirstBootWritesDefaults) {
    preferences._store.clear(); // simulate blank flash
    setVoltage = "99";          // should be overwritten
    readSettings();
    // After first-boot, defaults should be restored
    EXPECT_EQ(setVoltage, String("12"));
    EXPECT_EQ(microsteps, String("32"));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
