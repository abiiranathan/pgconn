CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -O3 -fPIC -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpq -pthread
SHARED_LDFLAGS = -shared
INSTALL_DIR = /usr/local

.PHONY: all library test clean install uninstall

all: library test

# Shared library target
library: libpgpool.so

libpgpool.so: pgpool.o
	$(CC) $(SHARED_LDFLAGS) -o $@ $^ $(LDFLAGS)

pgpool.o: pgpool.c pgpool.h
	$(CC) $(CFLAGS) -c $< -o $@

# Test program target
test: main.c libpgpool.so
	$(CC) $(CFLAGS) -L. -o $@ $< -lpgpool $(LDFLAGS)

# Installation targets
install: libpgpool.so
	install -d $(INSTALL_DIR)/lib
	install -m 644 libpgpool.so $(INSTALL_DIR)/lib
	install -d $(INSTALL_DIR)/include
	install -m 644 pgpool.h $(INSTALL_DIR)/include
	ldconfig

uninstall:
	rm -f $(INSTALL_DIR)/lib/libpgpool.so
	rm -f $(INSTALL_DIR)/include/pgpool.h
	ldconfig

clean:
	rm -f *.o *.so test
