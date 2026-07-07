# Native (off-target) firmware tests. Run from anywhere:
#   make -C firmware/native_tests
# Binaries expect to run from the repo root (golden vector paths).
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined

BUILD := build
REPO_ROOT := $(abspath $(CURDIR)/../..)

.PHONY: all test clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_protocol: test_protocol.cpp ../giga_bridge/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_protocol.cpp

$(BUILD)/fuzz_frames: fuzz_frames.cpp ../giga_bridge/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ fuzz_frames.cpp

test: $(BUILD)/test_protocol $(BUILD)/fuzz_frames
	cd $(REPO_ROOT) && firmware/native_tests/$(BUILD)/test_protocol
	cd $(REPO_ROOT) && firmware/native_tests/$(BUILD)/fuzz_frames

clean:
	rm -rf $(BUILD)
