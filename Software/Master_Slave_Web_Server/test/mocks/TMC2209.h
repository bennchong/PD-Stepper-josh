#pragma once
#include <stdint.h>

class HardwareSerial;

/**
 * @brief TMC2209 stepper driver stub.
 *
 * Tracks the last velocity commanded, enable/disable state, and
 * exposes a mutable Status so tests can inject fault conditions.
 */
class TMC2209 {
public:
    struct Status {
        bool over_temperature_warning  = false;
        bool over_temperature_shutdown = false;
    };
    enum StandstillMode { NORMAL=0, FREEWHEELING, BRAKING, STRONG_BRAKING };
    enum SerialAddress  { SERIAL_ADDRESS_0=0 };

    // Inspectable state
    int32_t  _velocity    = 0;
    bool     _enabled     = false;
    bool     _hw_disabled = false;
    uint32_t _stall_guard = 100;
    Status   _status;

    void setup(HardwareSerial &, long, int, uint8_t, uint8_t) {}
    void setRunCurrent(uint8_t)              {}
    void enableAutomaticCurrentScaling()     {}
    void enableStealthChop()                 {}
    void setCoolStepDurationThreshold(uint32_t) {}
    void disable()  { _enabled = false; }
    void enable()   { _enabled = true;  }
    void moveAtVelocity(int32_t v) { _velocity = v; }
    void setMicrostepsPerStep(uint16_t)      {}
    void setStallGuardThreshold(uint8_t)     {}
    void setStandstillMode(StandstillMode)   {}
    bool     hardwareDisabled()              { return _hw_disabled; }
    Status   getStatus()                     { return _status; }
    uint32_t getStallGuardResult()           { return _stall_guard; }
};
