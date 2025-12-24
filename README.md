wiegand-linux
=============

Userspace Wiegand reader for Wiren Board 8 (kernel 6.8, aarch64) using libgpiod + MQTT. Без ядрового модуля: два GPIO слушаются через IRQ, кадр Wiegand‑26 декодируется и публикуется в MQTT.

Defaults (WB8):
- D0: A2 IN (gpiochip0 line 228)
- D1: A1 IN (gpiochip0 line 233)

Architecture
- `wb-wiegand-mqtt`: userspace daemon (libgpiod + libmosquitto), слушает D0/D1, фильтр 100 мкс, тайм-аут кадра 50 мс, декодирует Wiegand‑26 (parity, Facility, Card).
- `wb-wiegand.conf`: конфиг (`/etc`), задаёт линии, DEVICE_ID, MQTT.
- `wiegand-gpiod-mqtt.service`: systemd unit (ExecStart=/usr/bin/wb-wiegand-mqtt --config /etc/wb-wiegand.conf).
- Kernel module/ DKMS остаются в дереве как legacy, но по умолчанию не собираются.

MQTT controls
- `/devices/<DEVICE_ID>/controls/ReadCounter`
- `/devices/<DEVICE_ID>/controls/Bits` (raw bit string)
- `/devices/<DEVICE_ID>/controls/Len`
- `/devices/<DEVICE_ID>/controls/Facility`
- `/devices/<DEVICE_ID>/controls/Card`
- `/devices/<DEVICE_ID>/controls/LastError` (`""`, `parity_fail`, `len_mismatch`)

### Build on WB8

```
apt update
apt install -y build-essential pkg-config libgpiod-dev libmosquitto-dev mosquitto
make              # builds wb-wiegand-mqtt
```

### Run manually (для проверки)
```
systemctl stop wb-mqtt-gpio          # либо уберите A1/A2 из его конфига
./wb-wiegand-mqtt --config ./wb-wiegand.conf &
mosquitto_sub -v -t "/devices/wiegand/#"
# проведите картой — увидите Bits/Facility/Card
```

### Install as service
```
make install
systemctl daemon-reload
systemctl enable --now wiegand-gpiod-mqtt.service
```

Конфиг в `/etc/wb-wiegand.conf` (меняйте D0/D1, DEVICE_ID, MQTT), затем `systemctl restart wiegand-gpiod-mqtt.service`.

### Совместимость с wb-mqtt-gpio
wb-mqtt-gpio и этот сервис не должны одновременно удерживать одни и те же линии. Исключите A1/A2 (или нужные пины) из `/etc/wb-mqtt-gpio.conf`, либо остановите wb-mqtt-gpio на время работы сервиса.
