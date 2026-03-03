CC ?= gcc
PREFIX ?= /usr/local
INSTALL_LIB_DIR = $(PREFIX)/lib
INSTALL_INCLUDE_DIR = $(PREFIX)/include

CFLAGS += -Wall -Wextra

TARGET_LOADABLE = jmmr0.so

ifeq ($(shell uname),Darwin)
  TARGET_LOADABLE = jmmr0.dylib
endif

$(TARGET_LOADABLE): sqlite-jmmr.c sqlite-jmmr.h
	$(CC) -fPIC -shared -O2 $(CFLAGS) $< -o $@

static: sqlite-jmmr.c sqlite-jmmr.h
	$(CC) -c -O2 -DSQLITE_CORE -DSQLITE_JMMR_STATIC $(CFLAGS) $< -o sqlite-jmmr.o

install: $(TARGET_LOADABLE) sqlite-jmmr.h
	install -d $(INSTALL_LIB_DIR)
	install -d $(INSTALL_INCLUDE_DIR)
	install -m 644 $(TARGET_LOADABLE) $(INSTALL_LIB_DIR)
	install -m 644 sqlite-jmmr.h $(INSTALL_INCLUDE_DIR)

test: $(TARGET_LOADABLE)
	sqlite3 :memory: '.load ./jmmr0' '.read tests/test_basic.sql'

clean:
	rm -f $(TARGET_LOADABLE) sqlite-jmmr.o

.PHONY: static install test clean
