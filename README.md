wiegand-linux
=============

Linux driver for reading Wiegand data from GPIO. Updated for Wiren Board 8 (kernel 6.8, aarch64) with a wb-mqtt bridge.

Defaults (WB8):
- D0: A2 IN (gpiochip0 line 228)
- D1: A1 IN (gpiochip0 line 233)

Components
- `wiegand-gpio.ko`: kernel module capturing bits on GPIO IRQs, exposes `/sys/kernel/wiegand/read`.
- `wb-wiegand-mqtt`: user-space daemon that loads the module, watches sysfs, decodes Wiegand-26 (facility + card), publishes MQTT controls.
- `wb-wiegand-mqtt.service`: systemd unit.
- `dkms.conf`: DKMS packaging for the module.

### Build on device (WB8)

```
apt install build-essential dkms pkg-config libmosquitto-dev
make            # builds module + wb-wiegand-mqtt
```

Load module manually:
```
modprobe wiegand-gpio D0=228 D1=233
```

Run bridge manually:
```
./wb-wiegand-mqtt --config ./wb-wiegand.conf
```

### Install with DKMS + service (manual steps)

```
dkms add .
dkms install -m wiegand-gpio -v 0.2.0
install -m644 wb-wiegand.conf /etc/wb-wiegand.conf
install -m755 wb-wiegand-mqtt /usr/bin/wb-wiegand-mqtt
install -m644 wb-wiegand-mqtt.service /lib/systemd/system/wb-wiegand-mqtt.service
systemctl daemon-reload
systemctl enable --now wb-wiegand-mqtt
```

### MQTT controls
- `/devices/wiegand/controls/ReadCounter`
- `/devices/wiegand/controls/Bits` (raw bit string)
- `/devices/wiegand/controls/Len`
- `/devices/wiegand/controls/FacilityCode`
- `/devices/wiegand/controls/CardNumber`
- `/devices/wiegand/controls/LastError`

Only Wiegand-26 frames are decoded; other lengths are reported with `LastError=len_mismatch`.
