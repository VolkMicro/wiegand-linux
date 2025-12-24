CC ?= gcc
MOSQ_CFLAGS := $(shell pkg-config --cflags libmosquitto 2>/dev/null)
MOSQ_LIBS := $(shell pkg-config --libs libmosquitto 2>/dev/null)
GPIOD_CFLAGS := $(shell pkg-config --cflags libgpiod 2>/dev/null)
GPIOD_LIBS := $(shell pkg-config --libs libgpiod 2>/dev/null)
MOSQ_LIBS ?= -lmosquitto
GPIOD_LIBS ?= -lgpiod
CFLAGS_USER ?= -O2 -Wall -Wextra
PREFIX ?= /usr
SYSCONFDIR ?= /etc
SYSTEMD_UNIT_DIR ?= /lib/systemd/system

.PHONY: all clean install

all: wb-wiegand-mqtt

wb-wiegand-mqtt: wb-wiegand-mqtt.c
	$(CC) $(CFLAGS_USER) $(MOSQ_CFLAGS) $(GPIOD_CFLAGS) -o $@ $< $(MOSQ_LIBS) $(GPIOD_LIBS)

clean:
	$(RM) wb-wiegand-mqtt

install: wb-wiegand-mqtt
	install -D -m755 wb-wiegand-mqtt $(DESTDIR)$(PREFIX)/bin/wb-wiegand-mqtt
	install -D -m644 wb-wiegand.conf $(DESTDIR)$(SYSCONFDIR)/wb-wiegand.conf
	install -D -m644 wiegand-gpiod-mqtt.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)/wiegand-gpiod-mqtt.service
