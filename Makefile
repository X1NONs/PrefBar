BUILD_HOST ?= build-host

.PHONY: all configure test payload clean distclean

# Default target builds the single combined PS5 release payload.
all: payload

configure:
	cmake -S . -B $(BUILD_HOST) -DCMAKE_BUILD_TYPE=Debug -DPREFBAR_BUILD_PS5=OFF

test: configure
	cmake --build $(BUILD_HOST) -j2
	ctest --test-dir $(BUILD_HOST) --output-on-failure

payload:
	bash scripts/scripts/build_ps5.sh

clean:
	rm -rf $(BUILD_HOST) build-ps5

distclean: clean
	rm -rf dist
