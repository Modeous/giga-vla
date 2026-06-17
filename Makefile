SHELL := /bin/bash
FQBN ?= arduino:mbed_giga:giga
SKETCH ?= firmware/test_harness
BUILD_DIR ?= build/$(notdir $(SKETCH))
VENV := .venv
PY := $(VENV)/bin/python

.PHONY: help setup build flash test sim-test iterate watch detect lint clean

help:
	@echo "make setup     - install arduino-cli, mbed_giga core, venv, python deps"
	@echo "make build     - compile $(SKETCH)"
	@echo "make flash     - compile + upload $(SKETCH) to attached GIGA"
	@echo "make test      - run HIL test suite (board must be flashed)"
	@echo "make sim-test  - run HIL suite against the software emulator (no board)"
	@echo "make iterate   - flash + test (one shot)"
	@echo "make watch     - flash + test on every change to firmware/ or tests/"
	@echo "make detect    - print detected GIGA port"
	@echo "make clean     - remove build/ and __pycache__"

setup:
	./scripts/setup_mac.sh

build:
	arduino-cli compile --fqbn $(FQBN) --output-dir $(BUILD_DIR) $(SKETCH)

flash:
	./scripts/flash.sh $(SKETCH)

test:
	$(VENV)/bin/pytest tests/ -v

sim-test:
	./scripts/run_sim_tests.sh -v

iterate:
	./scripts/iterate.sh

watch:
	./scripts/iterate.sh --watch

detect:
	$(PY) scripts/detect_giga.py --list

clean:
	rm -rf build __pycache__ tests/__pycache__ .pytest_cache report.xml report.html
