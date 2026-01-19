# OTA‑прошивка ESP32 (Hockey_Remote)

Актуальная, **проверенная на практике** инструкция OTA‑обновления прошивки ESP32 в проекте **Hockey_Remote**.

Инструкция ориентирована на:

* **Windows**
* **PowerShell**
* **VS Code + PlatformIO**
* OTA через **UDP (CMD_OTA_MODE) + ArduinoOTA / espota**

Без USB, Serial Monitor, mDNS и догадок.

---

## Ключевая идея

ESP32 **НЕ принимает OTA постоянно**.

Чтобы прошить плату, нужно строго выполнить цепочку:

1. Перевести ESP32 в **OTA‑режим** специальной UDP‑командой.
2. Получить **ACK**, подтверждающий вход в OTA‑режим.
3. **НЕМЕДЛЕННО** выполнить загрузку прошивки через PlatformIO (`espota`).

Если пропустить любой шаг — OTA **не сработает**.

---

## Обязательные условия

### Питание (критично)

⚠ **ESP32 должна питаться от стабильного источника 5 V (≥ 2 A)**.

Запрещено:

* прошивать от USB‑порта компьютера;
* прошивать при нестабильном питании.

Иначе возможны:

* зависание на `Sending invitation...`;
* `Listen Failed`;
* `No response from the ESP`.

---

## Что нужно знать заранее

* IP ESP32 (пример: `192.168.149.2`)
* UDP‑порт ESP32: `4210`
* OTA‑порт ESP32: `3232`
* IP компьютера **в той же Wi‑Fi сети**, что и ESP32

---

## Шаг 1. Узнать IP компьютера

В PowerShell:

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -like "192.168.149.*" -and $_.PrefixOrigin -ne "WellKnown" } |
  Select-Object IPAddress, InterfaceAlias
```

Пример:

```
IPAddress       InterfaceAlias
---------       --------------
192.168.149.102 Wi‑Fi
```

➡ Запомни IP компьютера (в примере `192.168.149.102`).

---

## Шаг 2. Проверить platformio.ini

Конфигурация **должна быть именно такой** (ключевой момент — `--host_ip`):

```ini
[platformio]
default_envs = esp32dev

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  crankyoldgit/IRremoteESP8266 @ ^2.8.6
  bblanchon/ArduinoJson@^7.4.2

upload_protocol = espota
upload_port = 192.168.149.2
upload_flags =
  --port=3232
  --host_ip=192.168.149.102
```

⚠ **Важно**:

* `--host_ip` — это **IP компьютера**, не ESP32.
* Переменные окружения **не использовать** (из‑за багов под Windows).

---

## Шаг 3. Перевести ESP32 в OTA‑режим и получить ACK

В PowerShell (одним блоком):

```powershell
$udp = New-Object System.Net.Sockets.UdpClient; \
$udp.Client.ReceiveTimeout = 2000; \
$udp.Connect("192.168.149.2", 4210); \
[byte[]]$pkt = 0xA5,0x01,0x70,0x20,0x00,0x00; \
$udp.Send($pkt, $pkt.Length) | Out-Null; \
try { $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0); $r = $udp.Receive([ref]$ep); "ACK: " + (($r | ForEach-Object { $_.ToString("X2") }) -join " ") } catch { "NO ACK (timeout)" }; \
$udp.Close()
```

Ожидаемый результат:

```
ACK: A5 01 7F 20 00 01 70
```

Расшифровка:

* `7F` — ACK
* `20 00` — ID (любой, главное уникальный)
* `01` — команда принята
* `70` — OTA‑режим

❌ **Если ACK нет — прошивку НЕ начинать.**

---

## Шаг 4. Немедленно прошить плату

⚠ **Не ждать**. OTA‑окно ограничено по времени.

В PowerShell:

```powershell
cd C:\Users\ch_73\Documents\PlatformIO\Projects\Hockey_Remote
pio run -e esp32dev -t upload
```

Или в VS Code:

* открыть проект;
* нажать **PlatformIO: Upload**.

---

## Что должно происходить при успешной OTA

* `espota.py` выводит `Starting on <IP компьютера>:<port>`
* идёт прогресс загрузки
* ESP32 перезагружается автоматически

После перезагрузки:

* OTA‑режим выключен;
* плата работает в штатном режиме.

---

## Типовые проблемы и решения

### Зависает на `Sending invitation...`

Причины:

* неверный `--host_ip`;
* ПК и ESP32 в разных сетях;
* нестабильное питание;
* OTA‑окно истекло.

Решение:

* снова отправить CMD_OTA_MODE;
* **сразу** повторить `pio run -t upload`.

---

### `Listen Failed`

Причина:

* в `--host_ip` подставилась строка вместо IP (переменные окружения).

Решение:

* прописывать IP компьютера **явно**.

---

### `No response from the ESP`

Причины:

* OTA‑режим не включён;
* Wi‑Fi client isolation;
* firewall Windows блокирует Python.

---

## Золотое правило OTA в этом проекте

**Каждая прошивка = новая сессия OTA**:

```
CMD_OTA_MODE  →  ACK  →  pio upload
```

Без сокращений.

---

## Итог

Этот порядок действий:

1. IP компьютера
2. `platformio.ini` с корректным `--host_ip`
3. CMD_OTA_MODE
4. ACK
5. `pio run -t upload`

— **единственный гарантированно рабочий сценарий OTA** для проекта *Hockey_Remote*.
