# ESP32-S3 Fan Remote Controller — Setup Guide

This guide walks you through building, flashing, and using the ESP32-S3 fan remote firmware on macOS.

---

## Hardware you need

| Item | Details |
|------|---------|
| ESP32-S3 N16R8 module | 16 MB flash, 8 MB OPI PSRAM |
| 433 MHz receiver module | Connected to **GPIO 6** |
| 433 MHz transmitter module | Connected to **GPIO 18** |
| Antennas | Two 17.3 cm straight wires (quarter-wave at 433 MHz) |
| USB-C cable | Data-capable (not charge-only) |

---

## 1 — Install VS Code

Download and install [Visual Studio Code](https://code.visualstudio.com/) for macOS.

---

## 2 — Install the PlatformIO extension

1. Open VS Code.
2. Click the **Extensions** icon in the sidebar (⇧⌘X).
3. Search for **PlatformIO IDE** and click **Install**.
4. Restart VS Code when prompted.

---

## 3 — Open the project

1. Clone or download this repository.
2. In VS Code: **File → Open Folder…** → select the `esp32-remote` folder.
3. PlatformIO will automatically detect `platformio.ini` and download the required toolchain and libraries on first open. This takes a few minutes.

---

## 4 — Connect the ESP32-S3

Plug the ESP32-S3 into your Mac via the **USB-C port**.

> **Important — First-time flashing (BOOT button):**
> The ESP32-S3 uses a built-in USB-Serial/JTAG controller instead of a separate UART chip. On some boards the first flash requires you to put the chip into download mode manually:
>
> 1. Hold the **BOOT** button on the board.
> 2. While holding BOOT, press and release the **RESET** (EN) button.
> 3. Release **BOOT**.
> 4. The board is now in download mode and PlatformIO can flash it.
>
> After the first successful flash, subsequent uploads work automatically without holding BOOT.

Verify the device appears:

```bash
ls /dev/cu.usb*
# Should show something like /dev/cu.usbmodem1101
```

If nothing appears, check:
- System Settings → Privacy & Security → allow the USB driver (macOS 13+).
- Try a different USB-C cable or port.

---

## 5 — Upload the filesystem (LittleFS)

The `data/` folder contains `config.json` (initially `{"fans": []}`). This must be uploaded to the LittleFS partition before or after flashing the firmware.

**Via PlatformIO sidebar:**
1. Click the **PlatformIO** alien-head icon in the VS Code sidebar.
2. Expand **esp32-s3-devkitc-1 → Platform**.
3. Click **Upload Filesystem Image**.

**Via PlatformIO terminal** (⌃\`):
```bash
pio run --target uploadfs
```

> If you get a "port not found" error, put the board into download mode (hold BOOT + press RESET) before running the command.

---

## 6 — Build and flash the firmware

**Via PlatformIO sidebar:**
- Click the **→ Upload** button (right-pointing arrow) in the bottom toolbar, or
- Expand **esp32-s3-devkitc-1 → General** → **Upload**.

**Via terminal:**
```bash
pio run --target upload
```

A successful flash ends with output like:
```
Wrote 1234567 bytes ... Hash of data verified.
Leaving...
Hard resetting via RTS pin...
```

---

## 7 — Find the ESP32's IP address

Open the Serial Monitor to see boot output:

**Via PlatformIO:**
- Click the **plug icon** (Serial Monitor) in the bottom toolbar, or
- Run `pio device monitor` in the terminal.

On a successful boot you will see:

```
========================================
  ESP32-S3 Fan Remote Controller
  Firmware build: May 17 2026 12:00:00
========================================

[config] LittleFS mounted
[config] Loaded 0 fan(s)
[rf] Initialised — TX GPIO18, RX GPIO6
[wifi] Connecting to SSID 'YourNetwork'...
[wifi] Connected! IP: 192.168.1.42
[api] HTTP server started on port 80

[main] System ready.
[main] API server:  http://192.168.1.42/
[main] Fans loaded: 0
```

Note the IP address — you'll use it for all API calls.

---

## 8 — Configure WiFi (first boot)

On first boot (no stored credentials) the device starts in **AP mode**:

```
[wifi] No stored credentials found
[wifi] Starting AP mode: SSID=ESP32-Fan-Remote
[wifi] AP IP: 192.168.4.1
[wifi] AP config server running at http://192.168.4.1
[wifi] Connect to 'ESP32-Fan-Remote' (password: fanremote123) to configure WiFi.
```

1. On your Mac, connect to the WiFi network **ESP32-Fan-Remote** (password: `fanremote123`).
2. Open a browser and go to **http://192.168.4.1**.
3. Enter your home WiFi SSID and password, then click **Save & Restart**.
4. The device will reboot and connect to your home network.
5. Reconnect your Mac to your home WiFi, then check the serial monitor for the assigned IP address.

---

## 9 — First API calls

Replace `192.168.1.42` with your device's actual IP throughout.

### Add a fan

```bash
curl -s -X POST http://192.168.1.42/fans/add \
  -H "Content-Type: application/json" \
  -d '{"name":"Living Room","max_speed":6,"lights":true}' | jq .
# → {"ok":true,"id":1}
```

### List fans

```bash
curl -s http://192.168.1.42/fans | jq .
```

### Learn a remote code

Point the original remote at the 433 MHz receiver and press the button within 30 seconds:

```bash
# Learn the "off" button
curl -s -X POST http://192.168.1.42/fans/1/learn/off | jq .
# → {"ok":true,"value":12345678,"pulse":350,"protocol":1,"bits":24}
```

Repeat for each command you want to control:
- `off`
- `speed_1` through `speed_6` (only up to the fan's `max_speed`)
- `light_on`, `light_off` (only if `lights: true`)

### Control the fan

```bash
# Turn off
curl -s -X POST http://192.168.1.42/fans/1/off | jq .

# Set speed 3
curl -s -X POST http://192.168.1.42/fans/1/speed/3 | jq .

# Turn light on
curl -s -X POST http://192.168.1.42/fans/1/light/on | jq .

# Turn light off
curl -s -X POST http://192.168.1.42/fans/1/light/off | jq .
```

### Update or delete a fan

```bash
# Rename and change max speed
curl -s -X POST http://192.168.1.42/fans/1/update \
  -H "Content-Type: application/json" \
  -d '{"name":"Bedroom","max_speed":3}' | jq .

# Delete
curl -s -X DELETE http://192.168.1.42/fans/1 | jq .
```

### Error codes

| HTTP | Meaning |
|------|---------|
| 200 | Success |
| 400 | Bad request (invalid parameters) |
| 404 | Fan not found |
| 408 | Timeout — no RF signal received within 30 s |
| 409 | Conflict — code not yet learned |
| 500 | Internal error |

---

## 10 — Remote access via Tailscale (MicroLink — future step)

The firmware currently serves the API on your local network only. Remote access (e.g. controlling the fan from outside your home) will be added in a future step using **Tailscale / MicroLink**, which creates a secure overlay network without exposing the device directly to the internet.

This step is not yet implemented.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Upload fails with "port not found" | Hold BOOT + press RESET to enter download mode |
| Filesystem upload fails | Same as above; also ensure no Serial Monitor is open (it locks the port) |
| Device connects to WiFi but no API response | Check firewall; ensure you're on the same subnet |
| `codes_learned` all false after reboot | Filesystem upload may have overwritten `config.json` — upload firmware first, then filesystem only once |
| RF learn times out | Ensure the remote is within ~1 m of the receiver and batteries are fresh |
| 433 MHz range is poor | Check that the 17.3 cm antenna wire is straight and fully soldered |

---

## Partition layout (reference)

| Name | Offset | Size | Purpose |
|------|--------|------|---------|
| nvs | 0x9000 | 20 KB | WiFi credentials, system NVS |
| otadata | 0xe000 | 8 KB | OTA slot tracking |
| app0 | 0x10000 | 3 MB | Main firmware (OTA slot 0) |
| app1 | 0x310000 | 3 MB | OTA update slot |
| spiffs | 0x610000 | ~9.9 MB | LittleFS (config, future web UI) |
