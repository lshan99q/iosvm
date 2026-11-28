# SPDX-License-Identifier: MIT
#
# xlt — the in-tree cross-architecture dynamic translator (M1).
#
#   make        build build/xlt
#   make test   build the test binaries and run tests/run_tests.sh
#   make clean  remove build/
#
# LLVM 18 is the code generator and capstone is the AArch64 decoder. Both are
# development-host dependencies and are never vendored into this tree.

CXX         ?= c++
LLVM_CONFIG ?= llvm-config-18

LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags)
LLVM_LIBDIR   := $(shell $(LLVM_CONFIG) --libdir)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Isrc
CXXFLAGS += $(LLVM_CXXFLAGS)
# llvm-config hands out -fno-exceptions. The engine's own error paths use
# exceptions even though LLVM does not, so put them back after LLVM's flags.
CXXFLAGS += -fexceptions

LDLIBS := -lLLVM-18 -lcapstone -lpthread -ldl -lz -ltinfo

BUILD := build
BIN   := $(BUILD)/xlt

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SRCS))

# Test binaries link the engine but not the CLI's main.cpp.
LIB_SRCS := $(filter-out src/main.cpp,$(SRCS))
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(LIB_SRCS))

TEST_SRCS := $(wildcard tests/*_test.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BUILD)/tests/%,$(TEST_SRCS))

.DEFAULT_GOAL := all
.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJS) | $(BUILD)
	$(CXX) $(LLVM_LDFLAGS) -Wl,-rpath,$(LLVM_LIBDIR) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/obj/%.o: src/%.cpp | $(BUILD)/obj
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/tests/%_test: tests/%_test.cpp $(LIB_OBJS) | $(BUILD)/tests
	$(CXX) $(CXXFLAGS) $(LLVM_LDFLAGS) -Wl,-rpath,$(LLVM_LIBDIR) -o $@ $< $(LIB_OBJS) $(LDLIBS)

test: $(BIN) $(TEST_BINS)
	@bash tests/run_tests.sh

$(BUILD) $(BUILD)/obj $(BUILD)/tests:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
