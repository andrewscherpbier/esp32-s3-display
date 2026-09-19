# esp32-s3-display

LVGL hello world for the Hosyond / LCDwiki **ES3C35P** — ESP32-S3R8 (8MB octal PSRAM,
16MB flash) with a 3.5" 320x480 ST77922 QSPI IPS panel and capacitive touch.

Built with ESP-IDF v5.5.5, `espressif/esp_lcd_st77922`, `espressif/esp_lvgl_port` and LVGL 9.

## Build and flash

```sh
. ~/esp/esp-idf-v5.5.5/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem31301 flash monitor
```

**The first time after flashing over other firmware, unplug the board and plug it back in.**
The panel's reset line is tied to the chip enable pin, and a reset over USB doesn't toggle
it, so the panel keeps the previous firmware's state and stays black. Later reflashes of this
app are fine.

## Board notes

- Display: `main/display.c` has the pinout and the panel init table decoded from the
  factory firmware (see <https://github.com/jlmeredith/ES3C35P>). RGB order, inversion on,
  4-pixel aligned draw windows, 40MHz QSPI.
- Backlight: GPIO41, active high, driven by LEDC PWM at 25kHz / 10-bit (high enough that
  the backlight driver's inductor doesn't whine). `display_set_brightness()` takes 0-100
  on a squared curve so the slider feels even.
- The panel is natively portrait; the app runs landscape (480x320) through LVGL software
  rotation (`sw_rotate`), since the ST77922 driver has no `swap_xy`. Flip it with
  `DISPLAY_ROTATION` in `main/display.h`. Touch needs no change — LVGL rotates the points.
- I²C: `main/board.c` owns the bus on SDA 38 / SCL 39. Touch (`0x55`), the ES8311
  codec (`0x18`) and the expansion header all share it; pass the handle around.
- Touch: `main/touch.c`. Sitronix controller at I²C `0x55` (RST 48), driven by
  `espressif/esp_lcd_touch_st7123` and polled by LVGL; INT (GPIO47) is unused.
  Reports come in the panel's native 320x480 frame.
- Audio: `main/audio.c`, via `espressif/esp_codec_dev`. ES8311 over I²S — MCLK 17,
  BCLK 18, WS 21, DOUT 15 (to codec), DIN 16 (from codec) — at 16kHz mono. Mic gain is
  at the driver's maximum (30dB analog PGA + 42dB ADC scale) and speech still only peaks
  around -25 dBFS, so recordings are normalized to -3 dBFS (capped at +24dB) before
  playback. The SC8002B amplifier is enabled by driving GPIO1 **LOW**. All of this was
  confirmed with an acoustic loopback test (880Hz tone out, mic in): the tone shows up
  in the mic only with GPIO1 low.
- RGB LED: `main/led.c`. One WS2812 on GPIO40, colour order GRB, driven by the RMT
  peripheral via `espressif/led_strip`. `led_set()` scales every channel to at most
  40/255 because full power is glaring; the home screen's LED button cycles
  Off/Red/Green/Blue/White.
- microSD: `main/sdcard.c` mounts FAT at `/sdcard` over 4-bit SDMMC — CLK 5, CMD 4,
  D0-D3 = 6/7/2/3 (the vendor's own SD example uses the same pins). The lines have
  external pull-ups. There's no card-detect pin, so a card is only picked up at boot, and
  the mount never formats a card. The status bar shows free space, or "none".
  Cards must use an **MBR** partition table with FAT/FAT32: IDF's FatFs is built without
  GPT support (`FF_LBA64 0`), so a GPT card mounts as "no filesystem". On a Mac:
  `diskutil eraseDisk FAT32 ESP32 MBRFormat /dev/diskN`.

## Wi-Fi and clock

- Tap the Wi-Fi label in the top-right of the home screen to open setup: it scans,
  lists nearby networks (strongest first), and takes the password on an on-screen
  keyboard. WPA/WPA2/WPA3-Personal and open networks are supported; enterprise and WEP
  networks are listed but disabled.
- `main/wifi.c` saves the network in NVS (namespace `wifi`) only after it connects, so a
  mistyped password never replaces a working network — after three rejected attempts
  the screen shows why and the board falls back to the saved network. A saved network
  that's unreachable is retried with backoff (2s up to 30s). "Forget" erases it.
- `main/clock.c` syncs time from `pool.ntp.org` each time Wi-Fi gets an address and shows
  it in the status bar. The timezone is the POSIX string `CLOCK_TZ` (US Pacific by
  default).
- Memory: Wi-Fi needs internal RAM next to LVGL's three DMA draw buffers, so the buffers
  are 32 lines each and the Wi-Fi driver's IRAM speed options are off
  (`sdkconfig.defaults`); Wi-Fi/lwIP buffers prefer PSRAM. About 87KB of internal RAM is
  free after boot (logged by `main`). The app partition is 4MB (`partitions.csv`).
