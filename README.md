wiegand-linux
=============

Userspace Wiegand reader for Wiren Board 8 (kernel 6.8, aarch64) using libgpiod + MQTT. Без ядрового модуля: два GPIO слушаются через IRQ, кадр Wiegand‑26 декодируется и публикуется в MQTT.

Defaults (WB8):
- D0: A2 IN (gpiochip0 line 228)
- D1: A1 IN (gpiochip0 line 233)

Architecture
- `wb-wiegand-mqtt`: userspace daemon (libgpiod + libmosquitto), слушает D0/D1, фильтр 100 мкс, тайм-аут кадра 50 мс, декодирует Wiegand‑26 и Wiegand‑34 (паритеты, Facility, Card). Пробует 4 варианта нормализации (as-is, invert, reverse, reverse+invert) чтобы учесть перепутанные линии/полярность.
- `wb-wiegand.conf`: конфиг (`/etc`), задаёт линии, DEVICE_ID, MQTT.
- `wiegand-gpiod-mqtt.service`: systemd unit (ExecStart=/usr/bin/wb-wiegand-mqtt --config /etc/wb-wiegand.conf).
- Kernel module/ DKMS остаются в дереве как legacy, но по умолчанию не собираются.

MQTT controls
- `/devices/<DEVICE_ID>/controls/ReadCounter`
- `/devices/<DEVICE_ID>/controls/Bits` (raw bit string)
- `/devices/<DEVICE_ID>/controls/Len`
- `/devices/<DEVICE_ID>/controls/Value` (raw bits as decimal)
- `/devices/<DEVICE_ID>/controls/Facility`
- `/devices/<DEVICE_ID>/controls/Card`
- `/devices/<DEVICE_ID>/controls/Format` (`w26`, `w34`, `unknown`)
- `/devices/<DEVICE_ID>/controls/LastError` (`""`, `parity_fail`, `len_mismatch`)

Formats decoded
- Wiegand‑26: bit0 even parity over 1..12, bit25 odd parity over 13..24; Facility=bits 1..8, Card=bits 9..24
- Wiegand‑34: bit0 even parity over 1..16, bit33 odd parity over 17..32; Facility=bits 1..16, Card=bits 17..32
Other lengths => `Format=unknown` и `LastError=len_mismatch`.

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
В linux gpiod-интерфейс требует эксклюзивного захвата линии. wb-mqtt-gpio и этот сервис не могут одновременно держать A1/A2. Варианты:
- Удалить/не описывать A1/A2 в `/etc/wb-mqtt-gpio.conf` (и `systemctl restart wb-mqtt-gpio`), тогда линии свободны для wiegand.
- Или остановить wb-mqtt-gpio целиком на устройстве, где нужен считыватель: `systemctl stop wb-mqtt-gpio`.
Проверить, что линии свободны: `gpioinfo gpiochip0 233 228` — `consumer` должен быть пустым (`unused`).
Если в логах `gpiod_line_request ... busy` — линии заняты, сервис завершится без захвата пинов.

### Конфигурация `/etc/wb-wiegand.conf`
```
DEVICE_ID=wiegand
D0=228              # A2 IN
D1=233              # A1 IN
MQTT_HOST=localhost
MQTT_PORT=1883
SKIP_META=0
# При необходимости поправить полярность/порядок:
SWAP_LINES=0
INVERT_BITS=0
REVERSE_BITS=0
```
После правок: `systemctl restart wiegand-gpiod-mqtt.service`.

### Диагностика / Troubleshooting
- Линии заняты: `journalctl` покажет `gpiod_line_request ... busy`. Проверьте `gpioinfo gpiochip0 233 228` — `consumer` должен быть пустой. Убейте старые `wb-wiegand-mqtt` (`pkill wb-wiegand-mqtt`), остановите или перенастройте wb-mqtt-gpio (убрать A1/A2), потом `systemctl restart wiegand-gpiod-mqtt`.
- Неверный формат: `LastError=len_mismatch`, `Format=unknown` — карта не 26/34 бит или в кадре шум/обрыв. Проверьте длину `Len` и `Bits`.
- Паритет не сходится: `LastError=parity_fail`. Попробуйте выставить в конфиге по одному: `SWAP_LINES=1`, затем `INVERT_BITS=1`, затем `REVERSE_BITS=1` (каждый раз рестарт сервиса) и смотрите, когда Facility/Card появляются.
- MQTT недоступен: убедитесь, что mosquitto запущен (`systemctl status mosquitto`).
- Проверка работы: `mosquitto_sub -v -t "/devices/<DEVICE_ID>/#"`; на валидной карте должны быть `Format=w26/w34`, `LastError` пустой, Facility/Card заполнены.

### Зачем опции нормализации
- SWAP_LINES — если D0/D1 физически перепутаны.
- INVERT_BITS — если уровни трактуются наоборот (импульс как лог.1).
- REVERSE_BITS — если порядок битов приходит задом наперёд.
Демон также сам пробует 4 комбинации (as-is/invert/reverse/reverse+invert) чтобы найти валидный паритет; ручные опции помогают закрепить правильный вариант.
