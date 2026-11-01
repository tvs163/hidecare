# Hidecare: сборка и прошивка

Hidecare — форк ESPectre. Используется **нативная прошивка на ESP-IDF** (`src/cpp/frontend/native`), не ESPHome. Датчик движения по Wi-Fi CSI работает как в ESPectre. Дополнительно прошивка регистрируется на сервере Hidecare и отправляет ему события.

## Что добавлено

| Что | Где |
| --- | --- |
| Клиент сервера Hidecare | `src/cpp/frontend/native/app/main/hidecare_client.{h,cpp}` |
| Настройки (Kconfig, меню «Hidecare») | `src/cpp/frontend/native/app/main/Kconfig.projbuild` |
| Подключение к прошивке | `app_main.cpp`, `native_frontend.{h,cpp}` (`set_motion_listener`) |

### Настройки

| Опция | По умолчанию | Назначение |
| --- | --- | --- |
| `CONFIG_HIDECARE_SERVER_URL` | `https://monitor.aidatastock.ru` | Адрес сервера, единственное место |
| `CONFIG_HIDECARE_DEVICE_CODE` | пусто | Пусто: `HC-` + последние 6 hex MAC (например `HC-A1B2C3`). Иначе используется заданный код |
| `CONFIG_HIDECARE_HEARTBEAT_SEC` | `300` | Интервал heartbeat |
| `CONFIG_HIDECARE_OTA_ENABLED` | `n` | Автоматическая проверка обновлений на сервере Hidecare (ручная работает всегда) |
| `CONFIG_HIDECARE_OTA_INTERVAL_SEC` | `21600` (6 ч) | Интервал проверки обновлений |
| `CONFIG_HIDECARE_SETUP_URL` | `https://t.me/hidecarebot` | Ссылка, которую Improv показывает после настройки Wi-Fi (раньше была espectre.dev) |

Код устройства печатается в лог при старте: `hidecare: device_code=HC-XXXXXX`.

### Протокол с сервером

1. После подключения к Wi-Fi: `POST /devices/register` `{"device_code","fw_version","mac"}` → `200 {"ok":true,"bound":bool}`. Если не удалось, повтор с backoff 5 с → 10 с → … → 10 мин. После каждого переподключения Wi-Fi регистрация повторяется.
2. События: `POST /events` `{"device_code","event","timestamp"}`, где `event` — `motion`, `no_motion` (смена состояния детектора) или `heartbeat` (раз в 5 минут). `timestamp` — ISO 8601 UTC по SNTP (`pool.ntp.org`); пока время не синхронизировано, поле не передаётся и сервер ставит время приёма. Комната не передаётся, сервер берёт её из привязки. Если сервер недоступен, событие повторяется каждые 5 с; очередь на 16 событий, при переполнении выбрасывается самое старое.
3. OTA: см. раздел «OTA» ниже.

## Что установить

ESP-IDF **5.5** (проверено на v5.5.1):

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3     # или esp32,esp32c3,esp32c6 ...
. ./export.sh                           # в каждом новом терминале
```

Можно также пользоваться штатным CLI проекта (`./espectre native build --chip s3`, см. `docs/CLI.md`), он собирает в Docker-контейнере ESP-IDF.

## Сборка

```bash
cd src/cpp/frontend/native/app
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3"   # свой чип
idf.py -B build-s3 -DSDKCONFIG=build-s3/sdkconfig set-target esp32s3
idf.py -B build-s3 -DSDKCONFIG=build-s3/sdkconfig menuconfig   # меню «Hidecare», по желанию
idf.py -B build-s3 -DSDKCONFIG=build-s3/sdkconfig build
```

Задать код устройства и включить OTA без menuconfig — добавить строки в свой файл defaults:

```
CONFIG_HIDECARE_DEVICE_CODE="HC-000123"
CONFIG_HIDECARE_OTA_ENABLED=y
```

и передать его последним в `SDKCONFIG_DEFAULTS`.

## Первая прошивка по USB

```bash
idf.py -B build-s3 -DSDKCONFIG=build-s3/sdkconfig -p /dev/ttyUSB0 flash monitor
```

Если плата не входит в режим прошивки: зажать BOOT, нажать RESET, отпустить BOOT. В Windows порт вида `COM5`.

## Настройка Wi-Fi

Captive portal (точка доступа с веб-страницей) в ESPectre нет. Wi-Fi настраивается по **Improv Serial** через тот же USB:

1. Открыть в Chrome/Edge https://www.improv-wifi.com/ (или ESPHome Web), «Connect», выбрать порт платы.
2. Ввести SSID и пароль. После подключения Improv покажет ссылку на бота.

Сброс Wi-Fi: удерживать BOOT 3 секунды, плата вернётся в режим настройки.

## Привязка в боте

1. В логе или в ответе сервера найти код устройства (`HC-XXXXXX`). Устройство само появится на сервере без владельца после регистрации.
2. В @hidecarebot: «Добавить устройство», ввести код и комнату. Бот вызывает `POST /devices/bind`.
3. Проверка: `curl https://monitor.aidatastock.ru/status/HC-XXXXXX` — видны `fw_version`, `mac`, `last_seen` и последнее событие.

## OTA

Единственный источник обновлений — сервер Hidecare: `CONFIG_HIDECARE_SERVER_URL` + `/firmware/latest`. Встроенный OTA-сервис ESPectre (`HttpsOtaService`) перенаправлен туда же: кнопки и команды ручной проверки/обновления в Device settings, Direct HTTP и MQTT ходят на сервер Hidecare, а не на GitHub. Канал (`release`/`preview`/`develop`) больше не влияет на адрес.

Порядок обновления на устройстве: запрос `GET /firmware/latest?device_code=..&version=..` → `204` (ничего нового) или `{"version","url","sha256"}` → если версия новее и `url` начинается с `https://`, образ пишется во второй OTA-раздел с подсчётом SHA-256 → при совпадении хеша раздел становится загрузочным → перезагрузка. При несовпадении образ отбрасывается.

С `CONFIG_HIDECARE_OTA_ENABLED=y` устройство само запускает обновление через минуту после старта и далее раз в `CONFIG_HIDECARE_OTA_INTERVAL_SEC` (6 часов). Без флага обновление ставится только вручную из Device settings.

### Как выложить новую версию

Файлы раздаёт Caddy: `https://monitor.aidatastock.ru/firmware/files/<имя>.bin` из каталога `/opt/monitor/firmware/files/` на сервере (листинга каталога нет). Манифест `/opt/monitor/firmware/latest.json` лежит вне раздаваемого каталога.

1. Поднять версию прошивки и собрать (см. «Сборка»). Образ для OTA — `build-s3/espectre-native.bin`.
2. Дать файлу имя с версией и чипом и посчитать хеш:
   ```bash
   cp build-s3/espectre-native.bin hidecare-3.1.1-esp32s3.bin
   sha256sum hidecare-3.1.1-esp32s3.bin
   ```
3. Загрузить на сервер:
   ```bash
   scp -i ~/.ssh/zetalink_ed25519 hidecare-3.1.1-esp32s3.bin root@82.114.228.137:/opt/monitor/firmware/files/
   curl -sI https://monitor.aidatastock.ru/firmware/files/hidecare-3.1.1-esp32s3.bin   # 200
   ```
4. Записать манифест на сервере (`sha256` — 64 hex из шага 2):
   ```bash
   ssh -i ~/.ssh/zetalink_ed25519 root@82.114.228.137 'cat > /opt/monitor/firmware/latest.json' <<'JSON'
   {"version": "3.1.1",
    "url": "https://monitor.aidatastock.ru/firmware/files/hidecare-3.1.1-esp32s3.bin",
    "sha256": "<64 hex>"}
   JSON
   curl -s "https://monitor.aidatastock.ru/firmware/latest?version=3.1.0"   # отдаёт манифест
   ```
5. Откатить выпуск: удалить `latest.json` — сервер снова отвечает 204.

Образ должен быть собран под тот же чип. Сервер пока не различает чипы, поэтому при нескольких моделях плат нужен отдельный манифест на чип.

## Ограничения

- Запросы к серверу без ключа устройства: кто знает код, может слать события от его имени.
- Device settings открывается со страницы espectre.dev (в прошивке разрешены её CORS-origins); сама прошивка на espectre.dev и GitHub не обращается.
- Captive portal не реализован, только Improv Serial по USB.
