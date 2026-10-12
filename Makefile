# Convenience wrapper around CMake. Everything here also works with plain
# cmake commands (see README.md).
BUILD ?= build
JOBS  ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all build test asan bench charts web-test demo clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j $(JOBS)

test: build
	cd $(BUILD) && ctest --output-on-failure

# AddressSanitizer + UndefinedBehaviorSanitizer, warnings as errors.
asan:
	cmake -S . -B $(BUILD)-asan -DCMAKE_BUILD_TYPE=Debug -DCACHESIM_SANITIZE=ON -DCACHESIM_WERROR=ON
	cmake --build $(BUILD)-asan -j $(JOBS)
	cd $(BUILD)-asan && ctest --output-on-failure

# Rewrite vs the original program (needs g++).
bench: build
	python3 tools/bench.py --bin $(BUILD)

# Regenerate docs/images/*.svg and docs/data/*.csv from real runs.
charts: build
	python3 tools/make_charts.py --bin $(BUILD)

# The JavaScript engine used by web/index.html must match the C++ engine.
web-test: build
	node web/test/crosscheck.mjs $(BUILD)/cachesim

demo: build
	$(BUILD)/cachesim -c configs/intel_like.cfg traces/example.trc
	$(BUILD)/tracegen matmul --n 64 | $(BUILD)/cachesim -q -s 8K -a 8 -
	$(BUILD)/tracegen matmul --n 64 --tile 8 | $(BUILD)/cachesim -q -s 8K -a 8 -

clean:
	rm -rf $(BUILD) $(BUILD)-asan
