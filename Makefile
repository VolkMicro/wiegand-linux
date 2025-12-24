KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := wiegand-gpio.o

CC ?= gcc
MOSQ_CFLAGS := $(shell pkg-config --cflags libmosquitto 2>/dev/null)
MOSQ_LIBS := $(shell pkg-config --libs libmosquitto 2>/dev/null)
MOSQ_LIBS ?= -lmosquitto
CFLAGS_USER ?= -O2 -Wall -Wextra

.PHONY: all module user clean

all: module wb-wiegand-mqtt

module: wiegand-gpio.c
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

wb-wiegand-mqtt: wb-wiegand-mqtt.c
	$(CC) $(CFLAGS_USER) $(MOSQ_CFLAGS) -o $@ $< $(MOSQ_LIBS)

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	$(RM) wb-wiegand-mqtt
