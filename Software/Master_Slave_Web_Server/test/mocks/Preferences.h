#pragma once
#include <map>
#include <string>
#include "WString.h"

/**
 * @brief In-memory Preferences stub.
 *
 * Stores key/value pairs in a std::map so tests can verify that
 * writeSettings() persists the correct values.
 */
class Preferences {
public:
    std::map<std::string,std::string> _store;
    bool _open = false;

    void begin(const char *, bool) { _open = true; }
    void end()                     { _open = false; }

    String getString(const char *key, const char *def="") const {
        auto it = _store.find(key);
        return it != _store.end() ? String(it->second.c_str()) : String(def);
    }
    void putString(const char *key, const String &val) {
        _store[key] = val.c_str();
    }
    void clear() { _store.clear(); }
};
