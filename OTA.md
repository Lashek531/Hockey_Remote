# OTA‑обновление ESP32 — подробная пошаговая инструкция

Документ описывает **полный и проверенный на практике процесс OTA‑обновления** прошивки ESP32 в проекте *Hockey_Remote*.

Инструкция рассчитана на:

* **Windows**
* **PowerShell**
* **PlatformIO**
* OTA через **UDP + ArduinoOTA (espota)**

Без USB, Serial Monitor, mDNS и догадок.

---

## Ключевая идея OTA в этом проекте

ESP32 **НЕ принимает OTA постоянно**.

Чтобы обновить прошивку, необходимо:

1. Явно перевести ESP32 в **сервисный OTA‑режим** по UDP.
2. Получить **ACK‑подтверждение**, что режим активирован.
3. Загрузить прошивку через PlatformIO (espota).

Если любой шаг пропущен — OTA **не заработает**.

---

## Требования

### Питание (критично)

⚠ **Обязательно внешнее питание 5 V (≥ 2 A)**.

Запрещено:

* прошивать от USB‑порта компьютера;
* прошивать при нестабильном питании.

Нарушение приводит к:

* `No response from the ESP`;
* обрывам OTA;
* зависаниям ESP32.

---

## Шаг 0. Что нужно знать заранее

* IP ESP32 (пример: `192.168.149.2`)
* UDP‑порт ESP32: `4210`
* OTA‑порт ESP32: `3232`

---

## Шаг 1. Открыть PowerShell

Используется **обычный PowerShell** (не обязательно от администратора).

⚠ Все команды ниже рекомендуется выполнять **в одном окне PowerShell**.

---

## Шаг 2. Узнать IP‑адрес компьютера

ESP32 должна уметь **обратиться к компьютеру**, поэтому нужно знать IP ПК в той же сети.

Выполните:

```powershell
Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -like "192.168.149.*" -and $_.PrefixOrigin -ne "WellKnown" } |
  Select-Object IPAddress, InterfaceAlias
```

Пример вывода:

```
IPAddress       InterfaceAlias
---------       --------------
192.168.149.102 Беспроводная сеть 2
```

➡ **Запомните IP компьютера** (в примере — `192.168.149.102`).

---

## Шаг 3. Задать переменную окружения ESPOTA_HOST_IP

PlatformIO использует эту переменную, чтобы указать ESP32, **куда подключаться для OTA**.

### Вариант A — временно (рекомендуется)

Действует только в текущем окне PowerShell:

```powershell
$env:ESPOTA_HOST_IP = "192.168.149.102"
```

### Вариант B — автоматически (удобно)

```powershell
$env:ESPOTA_HOST_IP = (
  Get-NetIPAddress -AddressFamily IPv4 |
  Where-Object { $_.IPAddress -like "192.168.149.*" } |
  Select-Object -First 1 -ExpandProperty IPAddress
)
```

### Вариант C — постоянно (опционально)

```powershell
setx ESPOTA_HOST_IP 192.168.149.102
```

После `setx` нужно **открыть новое окно PowerShell**.

---

## Шаг 4. Перевести ESP32 в OTA‑режим

OTA‑режим включается **специальной UDP‑командой** `CMD_OTA_MODE (0x70)`.

### Команда входа в OTA

```powershell
$udp = New-Object System.Net.Sockets.UdpClient
$udp.Client.ReceiveTimeout = 2000
$udp.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0))

$espIp   = "192.168.149.2"
$espPort = 4210

# UDP packet: MAGIC VER CMD ID(lo) ID(hi) LEN
[byte[]]$packet = 0xA5,0x01,0x70,0x01,0x00,0x00

$remote = [System.Net.IPEndPoint]::new([
  System.Net.IPAddress]::Parse($espIp),
  $espPort
)

$udp.Send($packet, $packet.Length) | Out-Null
```

---

## Шаг 5. Прочитать ACK‑ответ от ESP32

ESP32 **обязана** ответить ACK‑пакетом.

Добавьте в тот же PowerShell:

```powershell
try {
  $from = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
  $resp = $udp.Receive([ref]$from)
  "ACK: " + (($resp | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
}
catch {
  "ACK timeout"
}

$udp.Close()
```

### Ожидаемый результат

```
ACK: A5 01 7F 01 00 01 70
```

Расшифровка:

* `7F` — ACK
* `ID = 1` — совпадает с отправленным
* `STATUS = 1` — команда принята
* `CODE = 70` — подтверждение входа в OTA‑режим

❌ **Если ACK не пришёл — OTA невозможна.**

---

## Шаг 6. Загрузка прошивки через PlatformIO

Перейдите в каталог проекта:

```powershell
cd C:\Users\ch_73\Documents\PlatformIO\Projects\Hockey_Remote
```

Запустите загрузку:

```powershell
pio run -e esp32-c3-devkitm-1 -t upload
```

Во время загрузки:

* используется **espota**;
* USB‑порт не задействован;
* ESP32 подключается к компьютеру по Wi‑Fi.

---

## Шаг 7. Завершение OTA

После успешной загрузки:

* ESP32 автоматически перезагружается;
* OTA‑режим завершается;
* рабочая логика (IR + сирена) снова активна.

---

## Типовые проблемы и причины

### `No response from the ESP`

Причины:

* не задан `ESPOTA_HOST_IP`;
* Windows Firewall блокирует `python.exe`;
* ESP32 не в OTA‑режиме;
* нестабильное питание.

### ACK есть, OTA не начинается

Причины:

* неверный `upload_protocol` (должен быть `espota`);
* неверный env в PlatformIO;
* неверный IP компьютера.

---

## Итог

Этот порядок действий:

1. IP компьютера
2. `ESPOTA_HOST_IP`
3. `CMD_OTA_MODE`
4. ACK
5. `pio run -t upload`

— **единственный гарантированно рабочий сценарий OTA** для данного проекта.
