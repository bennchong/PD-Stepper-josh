#pragma once
#include <map>
#include <string>
#include <functional>
#include "WString.h"

#define HTTP_GET  1
#define HTTP_POST 2

// ── AsyncWebParameter ─────────────────────────────────────────────────────
class AsyncWebParameter {
    String _val;
public:
    explicit AsyncWebParameter(const String &v) : _val(v) {}
    const String &value() const { return _val; }
};

// ── AsyncWebServerRequest (minimal mock) ──────────────────────────────────
/**
 * @brief Minimal AsyncWebServerRequest stub.
 *
 * Tests populate params via setParam() before invoking route handlers.
 */
class AsyncWebServerRequest {
    std::map<std::string,AsyncWebParameter> _params;
public:
    bool hasParam(const char *name, bool isPost=false) const {
        return _params.count(name) > 0;
    }
    const AsyncWebParameter *getParam(const char *name, bool isPost=false) const {
        auto it = _params.find(name);
        return it != _params.end() ? &it->second : nullptr;
    }
    void send(int)                                     {}
    void send(int, const char *, const char *)         {}
    void send(int, const char *, const String &)       {}
    template<typename F>
    void send_P(int, const char *, const char *, F)    {}
    void redirect(const char *)                        {}

    // Test helpers
    void setParam(const char *name, const char *val) {
        _params.emplace(name, AsyncWebParameter{String(val)});
    }
    void clearParams() { _params.clear(); }
};

// ── AsyncWebServer ────────────────────────────────────────────────────────
using ArduinoHandler = std::function<void(AsyncWebServerRequest *)>;

class AsyncWebServer {
public:
    explicit AsyncWebServer(int) {}
    void on(const char *, int, ArduinoHandler) {}
    void begin() {}
};
