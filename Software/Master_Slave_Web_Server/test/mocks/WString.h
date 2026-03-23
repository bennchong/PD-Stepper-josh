#pragma once
#include <string>
#include <cstdlib>
#include <cstdio>
#include <iostream>

/**
 * @brief Minimal Arduino String wrapper around std::string.
 *
 * Provides the subset of the Arduino String API used by Master.ino and
 * Slave.ino so the sketches can be compiled and tested on a Linux host.
 */
class String {
    std::string _s;
public:
    String() {}
    String(const char *s)  : _s(s ? s : "") {}
    String(char c)         : _s(1, c) {}
    String(const String &o)      : _s(o._s) {}
    String(const std::string &s) : _s(s) {}
    String(int v)          { char b[32]; snprintf(b,32,"%d",v);   _s=b; }
    String(long v)         { char b[32]; snprintf(b,32,"%ld",v);  _s=b; }
    String(unsigned long v){ char b[32]; snprintf(b,32,"%lu",v);  _s=b; }
    String(uint32_t v)     { char b[32]; snprintf(b,32,"%u",v);   _s=b; }
    String(float  v, int d=2) { char b[32],f[8]; snprintf(f,8,"%%.%df",d); snprintf(b,32,f,(double)v); _s=b; }
    String(double v, int d=2) { char b[32],f[8]; snprintf(f,8,"%%.%df",d); snprintf(b,32,f,v); _s=b; }

    const char *c_str()  const { return _s.c_str(); }
    size_t length()      const { return _s.length(); }
    bool   isEmpty()     const { return _s.empty(); }
    int    toInt()       const { return atoi(_s.c_str()); }
    float  toFloat()     const { return (float)atof(_s.c_str()); }

    bool operator==(const String &o) const { return _s == o._s; }
    bool operator==(const char   *o) const { return _s == (o ? o : ""); }
    bool operator!=(const String &o) const { return !(*this==o); }
    bool operator!=(const char   *o) const { return !(*this==o); }
    bool operator< (const String &o) const { return _s  < o._s;  }

    String &operator=(const String &o) { _s = o._s;         return *this; }
    String &operator=(const char   *o) { _s = o ? o : "";   return *this; }

    String operator+(const String &o) const { return String(_s + o._s); }
    String operator+(const char   *o) const { return String(_s + (o ? o : "")); }
    String operator+(char c)          const { return String(_s + c); }

    String &operator+=(const String &o) { _s += o._s;        return *this; }
    String &operator+=(const char   *o) { if(o) _s += o;     return *this; }
    String &operator+=(char c)          { _s += c;            return *this; }

    char operator[](size_t i) const { return _s[i]; }

    operator std::string() const { return _s; }
    friend std::ostream &operator<<(std::ostream &os, const String &s) { return os << s._s; }
};

inline String operator+(const char *lhs, const String &rhs) {
    return String(std::string(lhs ? lhs : "") + rhs.c_str());
}
