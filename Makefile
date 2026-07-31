CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lz -lturbojpeg

BUILD_DIR := build
SRC := src/splashd.cpp
BIN := $(BUILD_DIR)/splashd
TEST_BIN := $(BUILD_DIR)/test_splashd

.PHONY: all test clean

all: $(BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN): $(SRC) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): tests/test_splashd.cpp $(SRC) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DSPLASHD_TEST -I. -o $@ $< $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)
