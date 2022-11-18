KERNEL_DIR=/usr/src/linux-headers-4.9.22-wb2

obj-m := wiegand-gpio.o
PWD := $(shell pwd)

all: wiegand-gpio.c
	$(MAKE) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	make -C $(KERNEL_DIR) SUBDIRS=$(PWD) clean

