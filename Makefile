CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/npit
CFLAGS ?= -O2
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcurl json-c libpng libjpeg openssl pangocairo fontconfig gio-2.0 libvlc) -DNPIT_DATADIR='"$(DATADIR)"'
CPPFLAGS += -DNPIT_PREFIX='"$(PREFIX)"' -DNPIT_BINDIR='"$(BINDIR)"'
LDLIBS += $(shell $(PKG_CONFIG) --libs libcurl json-c libpng libjpeg openssl pangocairo fontconfig gio-2.0 libvlc) -lm -pthread
OBJECTS = npit.o npit_mpris.o npit_http.o npit_lyrics.o npit_artwork.o npit_display.o npit_spotify.o npit_local.o npit_update.o

.PHONY: all clean install uninstall

all: npit

npit: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@

%.o: %.c npit_internal.h
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: npit
	install -Dm755 npit $(DESTDIR)$(BINDIR)/npit
	install -Dm644 settings.toml $(DESTDIR)$(DATADIR)/settings.toml
	install -Dm644 cava.conf $(DESTDIR)$(DATADIR)/cava.conf

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/npit
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -f npit $(OBJECTS)
