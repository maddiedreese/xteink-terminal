# XTeInk Terminal

Custom firmware and a tmux bridge for using an XTeInk X4 as a small Wi-Fi e-ink terminal display.

The basic shape:

- your keyboard stays paired to your computer
- your computer runs the real terminal inside `tmux`
- a Python bridge captures the tmux pane as plain text
- the bridge pushes changed frames to the X4 over Wi-Fi
- the X4 renders those frames on its e-ink display

This project replaces the stock reader firmware with native Arduino/PlatformIO firmware for the ESP32-C3 inside the X4. The firmware exposes a tiny HTTP API instead of trying to run a shell or language model on the device itself.

## Status

- Wi-Fi setup uses a WiFiManager captive portal.
- The X4 exposes:
  - `GET /status`
  - `POST /frame`
  - `GET /reset-wifi`
- Terminal rendering uses readable 2x bitmap text, about `56x28` cells.
- The tmux bridge converts common Unicode box-drawing characters to ASCII.
- The bridge wraps long lines before sending frames.
- Firmware attempts partial refresh for changed line bands after the first full refresh.

E-ink refresh is not instant. On this panel, full refreshes are roughly 1.7 seconds and partial refreshes are faster but still visibly e-ink.

## Hardware

Tested with:

- XTeInk X4
- ESP32-C3 native firmware
- Good Display `GDEQ0426T82` / SSD1677 e-ink panel via `GxEPD2`

The firmware uses these X4 display pins:

```cpp
EPD_SCLK = 8
EPD_MOSI = 10
EPD_MISO = 9
EPD_CS   = 21
EPD_DC   = 4
EPD_RST  = 5
EPD_BUSY = 6
```

## Firmware

Install PlatformIO, then build:

```sh
cd native-firmware/x4-terminal/firmware
pio run
```

Flash, replacing the serial port as needed:

```sh
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --before default-reset --baud 921600 write-flash -z \
  0x0 .pio/build/xteink_x4/bootloader.bin \
  0x8000 .pio/build/xteink_x4/partitions.bin \
  0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/xteink_x4/firmware.bin
```

If Wi-Fi is not configured, the X4 starts an access point named:

```txt
X4-Terminal-Setup
```

Join that AP and use the captive portal to choose a Wi-Fi network. Once connected, the display shows the device IP address. Use that IP as `http://X4_IP_ADDRESS` in the bridge command.

## Bridge

The supported bridge mirrors an existing tmux pane.

On the computer that can reach the X4 over Wi-Fi:

```sh
git clone https://github.com/maddiedreese/xteink-terminal.git
cd xteink-terminal
```

Install `tmux` if needed:

```sh
command -v tmux >/dev/null || brew install tmux
```

Create the terminal session:

```sh
tmux new -s x4-terminal
```

In another terminal, run the bridge:

```sh
python3 mac-bridge/x4_tmux_bridge.py --x4 http://X4_IP_ADDRESS --target x4-terminal: --cols 56 --rows 28
```

Anything displayed in that tmux session will be mirrored to the X4.

## Frame API

The X4 accepts frames as JSON:

```http
POST /frame
Content-Type: application/json
```

```json
{
  "cols": 56,
  "rows": ["hello from tmux"],
  "cursor": [0, 0]
}
```

`rows` should already be wrapped for the display size. The firmware clips any line that is still too long.

## Repository Layout

- `native-firmware/x4-terminal/firmware/src/main.cpp`: X4 firmware
- `native-firmware/x4-terminal/firmware/platformio.ini`: PlatformIO config
- `mac-bridge/x4_tmux_bridge.py`: tmux-to-X4 bridge

## Notes

- This is experimental custom firmware. Keep a backup of your original firmware if you may want to restore stock behavior.
- Stock X4 firmware may already use Wi-Fi for reader/app workflows. This project adds a general-purpose terminal frame endpoint.
- The X4 is a display endpoint here; terminal input and compute stay on your computer.
