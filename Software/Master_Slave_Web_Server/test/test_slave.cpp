/**
 * @file test_slave.cpp
 * @brief Native host unit tests for Slave.ino business logic.
 *
 * Compiles Slave.ino on a Linux host (via Arduino API mocks) and exercises
 * the pure-logic functions with Google Test.
 *
 * Build & run:
 * @code
 *   cd Software/Master_Slave_Web_Server/test
 *   make
 *   ./test_slave
 * @endcode
 */

#include <gtest/gtest.h>

// ── Instantiate mock global state ─────────────────────────────────────────
int           gpio_out[256]  = {};
int           gpio_in[256]   = {};
uint32_t      analog_mv[256] = {};
unsigned long _mock_millis   = 0;
unsigned long _mock_micros   = 0;

#include "mocks/Arduino.h"
#include "mocks/Wire.h"
#include "mocks/WiFi.h"
#include "mocks/Preferences.h"
#include "mocks/HTTPClient.h"
#include "mocks/TMC2209.h"
#include "mocks/AsyncTCP.h"
#include "mocks/ESPAsyncWebServer.h"

HardwareSerial Serial;
HardwareSerial Serial2;
TwoWire        Wire;
WiFiClass      WiFi;
int    HTTPClient::_mock_code = 200;
String HTTPClient::_mock_body = "OK";

// ── Include Slave.ino as a C++ translation unit ───────────────────────────
#include "../Slave/Slave.ino"

// =========================================================================
// readVoltage() – same formula as Master
// =========================================================================

TEST(SlaveVoltage, TwelveVoltReadingIsCorrect) {
    for (int i = 0; i < 256; i++) analog_mv[i] = 1427;
    String result = readVoltage();
    float v = atof(result.c_str());
    EXPECT_NEAR(v, 12.0f, 0.15f);
}

TEST(SlaveVoltage, ResultEndsWithV) {
    for (int i = 0; i < 256; i++) analog_mv[i] = 1000;
    EXPECT_EQ(readVoltage().c_str()[readVoltage().length()-1], 'V');
}

// =========================================================================
// readEncoder() – wrap-around detection (shared algorithm with Master)
//
// Same delta-based approach: static locals persist, so we only check changes.
// =========================================================================

class SlaveEncoderTest : public ::testing::Test {
protected:
    void readWith(int raw) {
        Wire.setResponse((uint8_t)(raw >> 8), (uint8_t)(raw & 0xFF));
        readEncoder();
    }
    long deltaAfter(int raw) {
        long before = total_encoder_counts;
        readWith(raw);
        return total_encoder_counts - before;
    }
};

TEST_F(SlaveEncoderTest, SameValueGivesZeroDelta) {
    readWith(1024);
    EXPECT_EQ(deltaAfter(1024), 0);
}

TEST_F(SlaveEncoderTest, CCWWrapDelta) {
    readWith(3800);
    EXPECT_EQ(deltaAfter(200), 200 - 3800 + 4096);
}

TEST_F(SlaveEncoderTest, CWWrapDelta) {
    readWith(200);
    EXPECT_EQ(deltaAfter(3800), 3800 - 200 - 4096);
}

// =========================================================================
// configureSettings() – USB-PD CFG pin truth table
// =========================================================================

class SlaveCfgPinTest : public ::testing::Test {
protected:
    void SetUp() override {
        memset(gpio_out, 0, sizeof(gpio_out));
        current        = "30";
        microsteps     = "32";
        stallThreshold = "10";
        standstillMode = "NORMAL";
    }
    void configure(const char *v) { setVoltage = v; configureSettings(); }
};

TEST_F(SlaveCfgPinTest, NineVolts)    { configure("9");  EXPECT_EQ(gpio_out[CFG1], LOW); EXPECT_EQ(gpio_out[CFG3], LOW); }
TEST_F(SlaveCfgPinTest, TwelveVolts) { configure("12"); EXPECT_EQ(gpio_out[CFG3], HIGH); }
TEST_F(SlaveCfgPinTest, TwentyVolts) { configure("20"); EXPECT_EQ(gpio_out[CFG2], HIGH); EXPECT_EQ(gpio_out[CFG3], LOW); }

// =========================================================================
// registerWithMaster() – does not attempt HTTP when WiFi is down
// =========================================================================

class RegisterTest : public ::testing::Test {
protected:
    void SetUp() override {
        registered             = false;
        HTTPClient::_mock_code = 200;
        HTTPClient::_mock_body = "OK";
    }
};

TEST_F(RegisterTest, SetsRegisteredTrueOn200) {
    WiFi._status = WL_CONNECTED;
    registerWithMaster();
    EXPECT_TRUE(registered);
}

TEST_F(RegisterTest, SkipsRegistrationWhenWiFiDown) {
    WiFi._status = WL_DISCONNECTED;
    registerWithMaster();
    EXPECT_FALSE(registered);
}

TEST_F(RegisterTest, RemainsUnregisteredOnNon200) {
    WiFi._status             = WL_CONNECTED;
    HTTPClient::_mock_code   = 500;
    registerWithMaster();
    EXPECT_FALSE(registered);
}

// =========================================================================
// loop() – pending settings are applied and written to flash
// =========================================================================

class SlaveSettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        preferences._store.clear();
        saveSettingsPending = false;
        speedUpdatePending  = false;
        posUpdatePending    = false;
        enabled1       = "enabled";
        setVoltage     = "12";
        microsteps     = "32";
        current        = "30";
        stallThreshold = "10";
        standstillMode = "NORMAL";
        WiFi._status   = WL_CONNECTED;
        registered     = true;
        _mock_millis   = 10000; // past wifiCheckInterval
        lastWiFiCheck  = 10000; // suppress WiFi check in loop
        lastEncRead    = 0;
        lastDebounceTime = 0;
        memset(gpio_out, 0, sizeof(gpio_out));
    }
};

TEST_F(SlaveSettingsTest, PendingSettingsAppliedInLoop) {
    pendingEnabled    = "disabled";
    pendingVoltage    = "20";
    pendingMicrosteps = "16";
    pendingCurrent    = "80";
    pendingStall      = "5";
    pendingStandstill = "FREEWHEELING";
    saveSettingsPending = true;

    loop();

    EXPECT_EQ(enabled1,       String("disabled"));
    EXPECT_EQ(setVoltage,     String("20"));
    EXPECT_EQ(microsteps,     String("16"));
    EXPECT_EQ(current,        String("80"));
    EXPECT_EQ(stallThreshold, String("5"));
    EXPECT_EQ(standstillMode, String("FREEWHEELING"));
    EXPECT_FALSE(saveSettingsPending);
}

TEST_F(SlaveSettingsTest, PendingSettingsPersistedToFlash) {
    pendingEnabled    = "enabled";
    pendingVoltage    = "15";
    pendingMicrosteps = "8";
    pendingCurrent    = "50";
    pendingStall      = "20";
    pendingStandstill = "BRAKING";
    saveSettingsPending = true;

    loop();

    // Verify Preferences mock was written
    EXPECT_EQ(preferences._store["voltage"],    "15");
    EXPECT_EQ(preferences._store["microsteps"], "8");
    EXPECT_EQ(preferences._store["current"],    "50");
}

// =========================================================================
// loop() – slave position step modes
// =========================================================================

class SlavePositionTest : public ::testing::Test {
protected:
    void SetUp() override {
        setPoint        = 0;
        CurrentPosition = 0;
        microsteps      = "32";
        enabled1        = "disabled";
        speedUpdatePending  = false;
        posUpdatePending    = false;
        saveSettingsPending = false;
        WiFi._status    = WL_CONNECTED;
        registered      = true;
        _mock_millis    = 10000;
        lastWiFiCheck   = 10000;
        lastEncRead     = 0;
        lastDebounceTime = 0;
    }
    void applyMode(int mode) {
        pendingPosMode   = mode;
        posUpdatePending = true;
        loop();
    }
};

TEST_F(SlavePositionTest, Mode1DecrementsByLargeStep) { applyMode(1); EXPECT_EQ(setPoint, -25600L); }
TEST_F(SlavePositionTest, Mode2DecrementsBySmallStep) { applyMode(2); EXPECT_EQ(setPoint, -12800L); }
TEST_F(SlavePositionTest, Mode3IncrementsBySmallStep) { applyMode(3); EXPECT_EQ(setPoint,  12800L); }
TEST_F(SlavePositionTest, Mode4IncrementsByLargeStep) { applyMode(4); EXPECT_EQ(setPoint,  25600L); }

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
