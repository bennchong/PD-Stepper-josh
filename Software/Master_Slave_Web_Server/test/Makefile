# Makefile for native host unit tests
#
# Prerequisites (Ubuntu / Debian):
#   sudo apt-get install build-essential libgtest-dev
#
# Usage:
#   make          # build both test binaries
#   make test     # build and run all tests
#   make clean    # remove build artefacts

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra \
            -I mocks \
            -I ../Master \
            -I ../Slave \
            -Wno-unused-variable \
            -Wno-unused-parameter \
            -Wno-unused-function \
            -Wno-sign-compare
LDFLAGS  := -lgtest -lgtest_main -lpthread

.PHONY: all test clean

all: test_master test_slave

# ── Master sketch test binary ─────────────────────────────────────────────
test_master: test_master.cpp mocks/Arduino.h mocks/WString.h mocks/Wire.h \
             mocks/WiFi.h mocks/Preferences.h mocks/HTTPClient.h          \
             mocks/TMC2209.h mocks/ESPAsyncWebServer.h mocks/AsyncTCP.h   \
             ../Master/Master.ino ../Master/master_index_html.h
	$(CXX) $(CXXFLAGS) test_master.cpp -o $@ $(LDFLAGS)

# ── Slave sketch test binary ──────────────────────────────────────────────
test_slave: test_slave.cpp mocks/Arduino.h mocks/WString.h mocks/Wire.h  \
            mocks/WiFi.h mocks/Preferences.h mocks/HTTPClient.h          \
            mocks/TMC2209.h mocks/ESPAsyncWebServer.h mocks/AsyncTCP.h   \
            ../Slave/Slave.ino
	$(CXX) $(CXXFLAGS) test_slave.cpp -o $@ $(LDFLAGS)

# ── Run all tests ─────────────────────────────────────────────────────────
test: all
	@echo "══════════════════════════════════════════"
	@echo "  Running Master tests"
	@echo "══════════════════════════════════════════"
	./test_master
	@echo ""
	@echo "══════════════════════════════════════════"
	@echo "  Running Slave tests"
	@echo "══════════════════════════════════════════"
	./test_slave

clean:
	rm -f test_master test_slave
