#pragma once
#include "WString.h"

// WiFi status codes (mirrors ESP32 SDK)
#define WL_CONNECTED     3
#define WL_DISCONNECTED  6
#define WIFI_STA         1
#define WIFI_AP          2
#define WIFI_AP_STA      3

struct IPAddress {
    uint8_t oct[4] = {192,168,4,2};
    String toString() const {
        char b[20];
        snprintf(b,20,"%d.%d.%d.%d",oct[0],oct[1],oct[2],oct[3]);
        return String(b);
    }
};

class WiFiClass {
public:
    int _status = WL_CONNECTED;

    void mode(int) {}
    void begin(const char *, const char *) {}
    int  status() const { return _status; }

    IPAddress localIP()  { return IPAddress(); }
    IPAddress softAPIP() { return IPAddress(); }
    bool softAP(const char *, const char *p="") { return true; }
    void reconnect() {}
};

extern WiFiClass WiFi;
