# Standalone Makefile — useful for developing on a Linux desktop.
# For OpenWRT packaging, use openwrt/Makefile with the SDK instead.

CC      ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=c11 -D_GNU_SOURCE
LDFLAGS ?=

# Required libraries.
PKGS := libmosquitto libcjson
PKG_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
PKG_LDLIBS  := $(shell $(PKG_CONFIG) --libs   $(PKGS) 2>/dev/null)

# libuci has no pkg-config file on some distros — link manually.
UCI_LDLIBS  := -luci

CFLAGS  += $(PKG_CFLAGS)
LDLIBS  += $(PKG_LDLIBS) $(UCI_LDLIBS)

SRC := src/main.c src/log.c src/config.c src/mqtt.c src/nft.c \
       src/handler.c src/leases.c
OBJ := $(SRC:.c=.o)
BIN := netparent

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
