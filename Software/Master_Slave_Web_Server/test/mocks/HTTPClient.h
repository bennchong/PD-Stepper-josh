#pragma once
#include "WString.h"

/**
 * @brief HTTPClient stub with configurable response code and body.
 *
 * Tests set _mock_code / _mock_body before exercising code that calls
 * slaveGet() / slavePost() / registerWithMaster().
 */
class HTTPClient {
public:
    static int    _mock_code;   ///< HTTP status to return from GET/POST
    static String _mock_body;   ///< Body to return from getString()

    void begin(const String &) {}
    void addHeader(const char *, const char *) {}
    void setTimeout(int) {}
    int    GET()             { return _mock_code; }
    int    POST(const String &) { return _mock_code; }
    String getString()       { return _mock_body; }
    void   end()             {}
};
