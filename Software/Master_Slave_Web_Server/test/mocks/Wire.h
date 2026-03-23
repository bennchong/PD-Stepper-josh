#pragma once
#include <stdint.h>

/**
 * @brief Minimal TwoWire (I2C) mock.
 *
 * Tests set _rx_buf[]/len before calling code that reads from the bus.
 * Calls to beginTransmission/write/endTransmission are silently ignored.
 */
class TwoWire {
public:
    uint8_t _rx_buf[16] = {};
    int     _rx_len     = 0;
    int     _rx_pos     = 0;

    void begin(int sda=-1, int scl=-1) {}
    void beginTransmission(uint8_t) {}
    void write(uint8_t) {}
    uint8_t endTransmission(bool=true) { return 0; }
    uint8_t requestFrom(uint8_t, uint8_t len) { _rx_pos=0; return len; }
    int     available() { return _rx_len - _rx_pos; }
    uint8_t read()      { return (_rx_pos < _rx_len) ? _rx_buf[_rx_pos++] : 0; }

    /// Helper for tests: load two bytes that the next requestFrom/read will return
    void setResponse(uint8_t hi, uint8_t lo) {
        _rx_buf[0]=hi; _rx_buf[1]=lo; _rx_len=2; _rx_pos=0;
    }
};

extern TwoWire Wire;
