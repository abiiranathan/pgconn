# Simple Makefile driver for CMake builds

# User-configurable variables (can be overridden via environment or CLI)
BUILD_DIR ?= build
BUILD_TYPE ?= Release
INSTALL_PREFIX ?= /usr/local
CC ?= gcc
CMAKE ?= cmake
GENERATOR ?=

# Library build type: STATIC, SHARED, or BOTH
LIB_TYPE ?= STATIC

# Optional extra flags passed to CMake
CFLAGS ?= -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=

# Internal: map LIB_TYPE to CMake's BUILD_SHARED_LIBS
# STATIC: OFF, SHARED: ON, BOTH: build both via separate targets
ifeq ($(LIB_TYPE),SHARED)
    BUILD_SHARED_LIBS := ON
else ifeq ($(LIB_TYPE),STATIC)
    BUILD_SHARED_LIBS := OFF
else ifneq ($(LIB_TYPE),BOTH)
    $(error Invalid LIB_TYPE=$(LIB_TYPE). Must be STATIC, SHARED, or BOTH)
endif

CMAKE_GENERATOR_FLAG := $(if $(GENERATOR),-G "$(GENERATOR)",)

# Base CMake configuration flags
define CMAKE_BASE_FLAGS
-S . -B $(1) \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_C_FLAGS="$(CFLAGS)" \
	-DCMAKE_SHARED_LINKER_FLAGS="$(LDFLAGS)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(LDFLAGS)"
endef

# Configuration for single library type builds
CMAKE_CONFIG_FLAGS := $(call CMAKE_BASE_FLAGS,$(BUILD_DIR)) \
	-DBUILD_SHARED_LIBS=$(BUILD_SHARED_LIBS)

.PHONY: all configure build debug release relwithdebinfo sanitize clean install uninstall help
.PHONY: static shared both

all: build

configure:
ifneq ($(LIB_TYPE),BOTH)
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(CMAKE_CONFIG_FLAGS)
else
	@echo "Configuring for BOTH static and shared libraries..."
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(call CMAKE_BASE_FLAGS,$(BUILD_DIR)-static) -DBUILD_SHARED_LIBS=OFF
	$(CMAKE) $(CMAKE_GENERATOR_FLAG) $(call CMAKE_BASE_FLAGS,$(BUILD_DIR)-shared) -DBUILD_SHARED_LIBS=ON
endif

build: configure
ifneq ($(LIB_TYPE),BOTH)
	$(CMAKE) --build $(BUILD_DIR) -j
else
	@echo "Building static library..."
	$(CMAKE) --build $(BUILD_DIR)-static -j
	@echo "Building shared library..."
	$(CMAKE) --build $(BUILD_DIR)-shared -j
endif

# Convenience targets for library type
static:
	$(MAKE) build LIB_TYPE=STATIC

shared:
	$(MAKE) build LIB_TYPE=SHARED

both:
	$(MAKE) build LIB_TYPE=BOTH

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
ifneq ($(LIB_TYPE),BOTH)
	$(CMAKE) --install $(BUILD_DIR)
else
	@echo "Installing static library..."
	$(CMAKE) --install $(BUILD_DIR)-static
	@echo "Installing shared library..."
	$(CMAKE) --install $(BUILD_DIR)-shared
endif

# Uninstall using CMake install manifest
uninstall:
ifneq ($(LIB_TYPE),BOTH)
	@if [ -f "$(BUILD_DIR)/install_manifest.txt" ]; then \
		xargs -a "$(BUILD_DIR)/install_manifest.txt" rm -f; \
		echo "Uninstalled files listed in install_manifest.txt"; \
	else \
		echo "No install_manifest.txt found in $(BUILD_DIR)."; \
		exit 1; \
	fi
else
	@if [ -f "$(BUILD_DIR)-static/install_manifest.txt" ]; then \
		xargs -a "$(BUILD_DIR)-static/install_manifest.txt" rm -f; \
		echo "Uninstalled static library files"; \
	fi
	@if [ -f "$(BUILD_DIR)-shared/install_manifest.txt" ]; then \
		xargs -a "$(BUILD_DIR)-shared/install_manifest.txt" rm -f; \
		echo "Uninstalled shared library files"; \
	fi
endif

clean:
ifneq ($(LIB_TYPE),BOTH)
	rm -rf $(BUILD_DIR)
else
	rm -rf $(BUILD_DIR)-static $(BUILD_DIR)-shared
endif

help:
	@echo "CMake build driver targets:"
	@echo "  all/build        - Configure and build (default LIB_TYPE=$(LIB_TYPE), BUILD_TYPE=$(BUILD_TYPE))"
	@echo "  configure        - Run CMake configure step"
	@echo "  static           - Build static library only"
	@echo "  shared           - Build shared library only"
	@echo "  both             - Build both static and shared libraries"
	@echo "  debug            - Build with Debug"
	@echo "  release          - Build with Release"
	@echo "  relwithdebinfo   - Build with RelWithDebInfo"
	@echo "  sanitize         - Build with AddressSanitizer"
	@echo "  install          - Install into $(INSTALL_PREFIX)"
	@echo "  uninstall        - Remove installed files via install_manifest.txt"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR        - Build directory (default: $(BUILD_DIR))"
	@echo "  BUILD_TYPE       - Release, Debug, RelWithDebInfo (default: $(BUILD_TYPE))"
	@echo "  LIB_TYPE         - STATIC, SHARED, or BOTH (default: $(LIB_TYPE))"
	@echo "  INSTALL_PREFIX   - Installation prefix (default: $(INSTALL_PREFIX))"
	@echo "  CC               - C compiler (default: $(CC))"
	@echo "  CFLAGS           - Compiler flags (default: $(CFLAGS))"
	@echo "  LDFLAGS          - Linker flags (default: $(LDFLAGS))"
	@echo "  GENERATOR        - CMake generator (default: system default)"
	@echo ""
	@echo "Examples:"
	@echo "  make                     # Build static library (default)"
	@echo "  make shared              # Build shared library"
	@echo "  make both                # Build both static and shared"
	@echo "  make LIB_TYPE=SHARED     # Same as 'make shared'"
	@echo "  make install LIB_TYPE=BOTH INSTALL_PREFIX=/opt/mylib"
	