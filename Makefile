# Compiler and tools
CC = gcc
INSTALL = install
RM = rm -f
LDCONFIG = ldconfig

# Directories
INSTALL_DIR = /usr/local
LIB_DIR = $(INSTALL_DIR)/lib
INCLUDE_DIR = $(INSTALL_DIR)/include
BUILD_DIR = build

# Library info
LIB_NAME = pgpool
LIB_VERSION = 1.0.0
LIB_SONAME = lib$(LIB_NAME).so
LIB_TARGET = $(LIB_SONAME).$(LIB_VERSION)
LIB_LINK = $(LIB_SONAME).1

# Source and object files
SOURCES = pgpool.c pgtypes.c
OBJECTS = $(addprefix $(BUILD_DIR)/, $(SOURCES:.c=.o))
DEPS = $(OBJECTS:.o=.d)
HEADERS = pgpool.h pgtypes.h pgiter.h
TEST_SOURCE = main.c
TEST_TARGET = test

# Compiler flags
CFLAGS = -Wall -Wextra -Werror -pedantic -O3 -fPIC -std=c23 -D_GNU_SOURCE
CPPFLAGS = 
LDFLAGS = -lpq -pthread
SHARED_LDFLAGS = -shared -Wl,-soname,$(LIB_LINK)

# Installation flags
INSTALL_LIB_FLAGS = -m 755
INSTALL_HEADER_FLAGS = -m 644

.PHONY: all library test clean install uninstall help

all: library test

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Shared library target
library: $(LIB_TARGET)

$(LIB_TARGET): $(OBJECTS)
	$(CC) $(SHARED_LDFLAGS) -o $@ $^ $(LDFLAGS)
	ln -sf $(LIB_TARGET) $(LIB_SONAME)
	ln -sf $(LIB_TARGET) $(LIB_LINK)

# Object file compilation
$(BUILD_DIR)/%.o: %.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Test program target
$(TEST_TARGET): $(TEST_SOURCE) $(LIB_TARGET)
	$(CC) $(CFLAGS) -L. -o $@ $< -l$(LIB_NAME) $(LDFLAGS)

test: $(TEST_TARGET)

# Installation targets
install: $(LIB_TARGET)
	$(INSTALL) -d $(LIB_DIR)
	$(INSTALL) $(INSTALL_LIB_FLAGS) $(LIB_TARGET) $(LIB_DIR)
	ln -sf $(LIB_TARGET) $(LIB_DIR)/$(LIB_SONAME)
	ln -sf $(LIB_TARGET) $(LIB_DIR)/$(LIB_LINK)
	$(INSTALL) -d $(INCLUDE_DIR)
	$(INSTALL) $(INSTALL_HEADER_FLAGS) $(HEADERS) $(INCLUDE_DIR)
	$(LDCONFIG)

uninstall:
	$(RM) $(LIB_DIR)/$(LIB_TARGET)
	$(RM) $(LIB_DIR)/$(LIB_SONAME)
	$(RM) $(LIB_DIR)/$(LIB_LINK)
	$(RM) $(addprefix $(INCLUDE_DIR)/, $(HEADERS))
	$(LDCONFIG)

clean:
	$(RM) -r $(BUILD_DIR) $(LIB_TARGET) $(LIB_SONAME) $(LIB_LINK) $(TEST_TARGET)

# Development targets
debug: CFLAGS += -g -DDEBUG -O0
debug: clean all

release: CFLAGS += -DNDEBUG
release: clean all

# Package creation
PKG_NAME = lib$(LIB_NAME)-$(LIB_VERSION)
PKG_FILES = $(SOURCES) $(HEADERS) $(TEST_SOURCE) Makefile README.md

package: $(PKG_NAME).tar.gz

$(PKG_NAME).tar.gz: $(PKG_FILES)
	tar -czf $@ --transform 's,^,$(PKG_NAME)/,' $^

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build library and test program (default)"
	@echo "  library   - Build shared library only"
	@echo "  test      - Build test program"
	@echo "  debug     - Build with debug symbols and no optimization"
	@echo "  release   - Build optimized release version"
	@echo "  install   - Install library and headers to $(INSTALL_DIR)"
	@echo "  uninstall - Remove installed files"
	@echo "  package   - Create source tarball"
	@echo "  clean     - Remove built files"
	@echo "  help      - Show this help message"

# Dependency tracking
-include $(DEPS)

$(BUILD_DIR)/%.d: %.c | $(BUILD_DIR)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -MM $< | sed 's,\($*\)\.o[ :]*,$(BUILD_DIR)/\1.o $@ : ,g' > $@