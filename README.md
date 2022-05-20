wiegand-linux
=============

Linux driver for reading wiegand data from GPIO. Tested on Wiren Board 4 (Freescale i.mx233) and Wiren Board 6 (NXP i.MX 6ULL)


- Ensure your arm-linux cross compiler is in your path.
- Ensure your kernel is at the same level as this directory.

Repository contains built kernel module for armhf (Wiren Board 6, Kernel 4.9.22-wb6 +wb20200610110035)

### Installation

- Check kernel version with `apt show linux-image-wb6 | grep Version`
- Clone or download this repository
- Change GPIO's ids in file `wiegand-monitor`, if you need
- Exec `wiegand-monitor` script

MQTT virtual device will be available at `/devices/wiegand/controls/Reader`


### Building from source

To build this module from source for Wiren Board kernel we need [WB Dev Environment](https://github.com/wirenboard/wirenboard)

- Clone repo
- Run `WBDEV_TARGET=wb6 wbdev chuser` in project directory (wb6 for Wiren Board 6)
- `make`
