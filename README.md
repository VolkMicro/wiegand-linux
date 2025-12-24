wiegand-linux
==============

Пользовательский Wiegand-считыватель для Wiren Board 8 (kernel 6.8, aarch64) на libgpiod + MQTT. Ядровой модуль не используется: две линии GPIO слушаются по IRQ, кадры Wiegand‑26 и Wiegand‑34 декодируются в Facility/Card, результаты публикуются в MQTT.

Поддерживаемые форматы
- Wiegand-26: бит0 чётный паритет по битам 1..12, бит25 — нечётный по 13..24; Facility=1..8, Card=9..24.
- Wiegand-34: бит0 чётный по 1..16, бит33 — нечётный по 17..32; Facility=1..16, Card=17..32.
Другие длины -> Format=unknown, LastError=len_mismatch.

MQTT-топики (`DEVICE_ID` из конфига)
- `/devices/DEVICE_ID/controls/ReadCounter`
- `/devices/DEVICE_ID/controls/Bits` (сырые биты)
- `/devices/DEVICE_ID/controls/Len`
- `/devices/DEVICE_ID/controls/Value` (сырые биты в десятичном)
- `/devices/DEVICE_ID/controls/Facility`
- `/devices/DEVICE_ID/controls/Card`
- `/devices/DEVICE_ID/controls/Format` (`w26`, `w34`, `unknown`)
- `/devices/DEVICE_ID/controls/LastError` (`""`, `parity_fail`, `len_mismatch`)

Зависимости
- build-essential, pkg-config
- libgpiod-dev, libmosquitto-dev, mosquitto

GPIO по умолчанию (WB8)
- D0: A2 IN (gpiochip0 line 228)
- D1: A1 IN (gpiochip0 line 233)

Быстрая установка (шаги)
1) Установить зависимости:
```
apt update
apt install -y git build-essential pkg-config libgpiod-dev libmosquitto-dev mosquitto
```
2) Клонировать и собрать:
```
cd /mnt/data
git clone https://github.com/VolkMicro/wiegand-linux
cd wiegand-linux
make
```
3) Убедиться, что A1/A2 свободны: либо остановить wb-mqtt-gpio, либо убрать эти пины из `/etc/wb-mqtt-gpio.conf`. Проверка:
```
gpioinfo gpiochip0 233 228   # consumer должен быть unused
```
4) Установить сервис:
```
make install
systemctl daemon-reload
systemctl enable --now wiegand-gpiod-mqtt.service
```
5) Проверка:
```
mosquitto_sub -v -t "/devices/wiegand/#"
```

Конфигурация `/etc/wb-wiegand.conf`
```
DEVICE_ID=wiegand
D0=228              # A2 IN
D1=233              # A1 IN
MQTT_HOST=localhost
MQTT_PORT=1883
SKIP_META=0
# Опции нормализации, если перепутаны линии/полярность/порядок:
SWAP_LINES=0
INVERT_BITS=0
REVERSE_BITS=0
```
После правок: `systemctl restart wiegand-gpiod-mqtt.service`.

Совместимость с wb-mqtt-gpio
- gpiod требует эксклюзивный захват линий. wb-mqtt-gpio и wb-wiegand-mqtt не могут одновременно держать A1/A2.
- Уберите A1/A2 из `/etc/wb-mqtt-gpio.conf` и перезапустите wb-mqtt-gpio, либо остановите wb-mqtt-gpio (`systemctl stop wb-mqtt-gpio`) на устройстве, где работает считыватель.
- Если в логах `gpiod_line_request ... busy`, линии заняты и сервис выйдет.

Диагностика
- Логи сервиса: `journalctl -u wiegand-gpiod-mqtt.service -n 50`.
- Проверка занятости: `gpioinfo gpiochip0 233 228` — `consumer` должен быть пустой.
- Ошибки паритета/длины: смотрите `LastError`, `Format`, `Len`, `Bits`. При `parity_fail` можно поочерёдно включать SWAP_LINES, INVERT_BITS, REVERSE_BITS (каждый раз с перезапуском) и выбрать вариант, где Facility/Card заполняются.
- MQTT доступность: `systemctl status mosquitto`.

Ручной запуск (без systemd)
```
systemctl stop wb-mqtt-gpio   # если нужно освободить пины
./wb-wiegand-mqtt --config ./wb-wiegand.conf &
mosquitto_sub -v -t "/devices/wiegand/#"
```

Устройство
- Демон слушает два GPIO через libgpiod (IRQ), фильтр по дребезгу 100 мкс, таймаут кадра 50 мс.
- Для каждого кадра пробует 4 нормализации (как есть, инверсия, реверс, реверс+инверсия) и выбирает ту, где сходится паритет (для w26/w34).

