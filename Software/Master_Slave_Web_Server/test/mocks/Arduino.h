#pragma once
#include <stdint.h>
#include <math.h>
#include <iostream>
#include "WString.h"

// ── Pin / level constants ─────────────────────────────────────────────────
#define HIGH 1
#define LOW  0
#define INPUT      0
#define OUTPUT     1
#define INPUT_PULLUP 2

// ── ADC attenuation (ESP32) ───────────────────────────────────────────────
#define ADC_11db 3

// ── Arduino flash-storage qualifier (no-op on host) ──────────────────────
#define PROGMEM

// ── I2C default pins (ESP32-S3) ───────────────────────────────────────────
#define SDA 8
#define SCL 9

// ── Arithmetic helpers ────────────────────────────────────────────────────
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

// ── Types ─────────────────────────────────────────────────────────────────
typedef uint8_t  byte;
typedef uint16_t word;
typedef bool     boolean;

// ── Controllable mock state (defined in each test .cpp) ──────────────────
extern int           gpio_out[256]; ///< Written by digitalWrite(); checked in tests
extern int           gpio_in[256];  ///< Read by digitalRead(); set by tests
extern uint32_t      analog_mv[256];///< Read by analogReadMilliVolts(); set by tests
extern unsigned long _mock_millis;  ///< Returned by millis(); advance in tests
extern unsigned long _mock_micros;  ///< Returned by micros(); advance in tests

// ── Time functions ────────────────────────────────────────────────────────
inline unsigned long millis()  { return _mock_millis; }
inline unsigned long micros()  { return _mock_micros; }
inline void delay(unsigned long ms) { _mock_millis += ms; }
inline void delayMicroseconds(unsigned long us) { _mock_micros += us; }

// ── GPIO ──────────────────────────────────────────────────────────────────
inline void    pinMode(uint8_t, uint8_t) {}
inline void    digitalWrite(uint8_t pin, uint8_t val) { gpio_out[pin] = val; }
inline int     digitalRead(uint8_t pin)               { return gpio_in[pin]; }
inline uint32_t analogReadMilliVolts(uint8_t pin)     { return analog_mv[pin]; }
inline void    analogSetPinAttenuation(uint8_t, int)  {}

// ── HardwareSerial stub ───────────────────────────────────────────────────
class HardwareSerial {
public:
    void begin(long) {}
    template<typename T> void print(T)   {}
    template<typename T> void println(T) {}
    void println() {}
    operator bool() const { return true; }
};

extern HardwareSerial Serial;
extern HardwareSerial Serial2;
