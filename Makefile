# Simple Makefile driver for CMake builds

# User-configurable variables (can be overridden via environment or CLI)
BUILD_DIR ?= build
BUILD_TYPE ?= Release
INSTALL_PREFIX ?= /usr/local
CC ?= gcc
CMAKE ?= cmake
GENERATOR ?=

# Optional extra flags passed to CMake
CFLAGS ?=-Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

# Internal
CMAKE_GENERATOR_FLAG := $(if $(GENERATOR),-G "$(GENERATOR)",)
CMAKE_CONFIG_FLAGS := -S . -B $(BUILD_DIR) \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_C_FLAGS="$(CFLAGS)" \
	-DCMAKE_SHARED_LINKER_FLAGS="$(LDFLAGS)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(LDFLAGS)"

.PHONY: all configure build debug release relwithdebinfo sanitize clean install uninstall help

all: build

configure:
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_CONFIG_FLAGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j

debug:
	$(MAKE) build BUILD_TYPE=Debug

release:
	$(MAKE) build BUILD_TYPE=Release

relwithdebinfo:
	$(MAKE) build BUILD_TYPE=RelWithDebInfo

# Example sanitizer build (AddressSanitizer); can be customized further
sanitize:
	$(MAKE) build BUILD_TYPE=Debug CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address"

install: build
	$(CMAKE) --install $(BUILD_DIR)

# Uninstall using CMake install manifest
uninstall:
	@if [ -f "$(BUILD_DIR)/install_manifest.txt" ]; then \
		xargs -a "$(BUILD_DIR)/install_manifest.txt" rm -f; \
		echo "Uninstalled files listed in install_manifest.txt"; \
	else \
		echo "No install_manifest.txt found in $(BUILD_DIR)."; \
		exit 1; \
	fi

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "CMake build driver targets:"
	@echo "  all/build        - Configure and build (default BUILD_TYPE=$(BUILD_TYPE))"
	@echo "  configure        - Run CMake configure step"
	@echo "  debug            - Build with Debug"
	@echo "  release          - Build with Release"
	@echo "  relwithdebinfo   - Build with RelWithDebInfo"
	@echo "  sanitize         - Build with AddressSanitizer"
	@echo "  install          - Install into $(INSTALL_PREFIX)"
	@echo "  uninstall        - Remove installed files via install_manifest.txt"
	@echo "Variables: BUILD_DIR, BUILD_TYPE, INSTALL_PREFIX, CC, CFLAGS, LDFLAGS, GENERATOR"