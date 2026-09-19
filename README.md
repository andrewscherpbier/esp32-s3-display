# esp32-s3-display

ESP-IDF projects for the Hosyond / LCDwiki **ES3C35P** — ESP32-S3R8 (8MB octal PSRAM,
16MB flash) with a 3.5" 320x480 ST77922 QSPI IPS panel, capacitive touch, ES8311 audio,
a WS2812 RGB LED and a microSD slot.

Built with ESP-IDF v5.5.5 and LVGL 9.

## Layout

```
components/
  es3c35p_bsp/   board support: I2C bus, display + LVGL + backlight, touch, audio codec,
                 RGB LED, microSD. All GPIOs are in include/bsp_pins.h
  net/           Wi-Fi station with on-screen setup (LVGL), NTP clock
apps/
  demo/          hardware demo: touch, beep / record / playback, mic meter, volume,
                 brightness, LED colours, SD free space, Wi-Fi + clock status bar
common/
  sdkconfig.defaults   settings every app needs (PSRAM, flash, RAM tuning, fonts)
  partitions.csv
```

Each app is its own ESP-IDF project that pulls in `components/` and `common/` from its
`CMakeLists.txt`. To start a new one, copy `apps/demo/CMakeLists.txt`, rename the
project, and list the components it uses in its `main/CMakeLists.txt`.

## Build and flash

```sh
. ~/esp/esp-idf-v5.5.5/export.sh
cd apps/demo
idf.py build
idf.py -p /dev/cu.usbmodem31301 flash monitor
```

**The first time after flashing over other firmware, unplug the board and plug it back in.**
The panel's reset line is tied to the chip enable pin, and a reset over USB doesn't toggle
it, so the panel keeps the previous firmware's state and stays black. Later reflashes are
fine.

## Board notes

- Display: `bsp_display.c` has the panel init table decoded from the
  factory firmware (see <https://github.com/jlmeredith/ES3C35P>). RGB order, inversion on,
  4-pixel aligned draw windows, 40MHz QSPI.
- Backlight: GPIO41, active high, driven by LEDC PWM at 25kHz / 10-bit (high enough that
  the backlight driver's inductor doesn't whine). `bsp_display_set_brightness()` takes 0-100
  on a squared curve so the slider feels even.
- The panel is natively portrait; the demo runs landscape (480x320) through LVGL software
  rotation (`sw_rotate`), since the ST77922 driver has no `swap_xy`. Apps pick the
  rotation in `bsp_display_init()`. Touch needs no change — LVGL rotates the points.
- I²C: `bsp_i2c.c` owns the bus on SDA 38 / SCL 39. Touch (`0x55`), the ES8311
  codec (`0x18`) and the expansion header all share it; pass the handle around.
- Touch: `bsp_touch.c`. Sitronix controller at I²C `0x55` (RST 48), driven by
  `espressif/esp_lcd_touch_st7123` and polled by LVGL; INT (GPIO47) is unused.
  Reports come in the panel's native 320x480 frame.
- Audio: `bsp_audio.c` opens the ES8311 via `espressif/esp_codec_dev` and returns the
  codec handle. I²S: MCLK 17, BCLK 18, WS 21, DOUT 15 (to codec), DIN 16 (from codec).
  Mic gain is at the driver's maximum (30dB analog PGA + 42dB ADC scale) and speech still
  only peaks around -25 dBFS, so the demo normalizes recordings to -3 dBFS (capped at
  +24dB) before playback. The SC8002B amplifier is enabled by driving GPIO1 **LOW**. All of this was
  confirmed with an acoustic loopback test (880Hz tone out, mic in): the tone shows up
  in the mic only with GPIO1 low.
- RGB LED: `bsp_led.c`. One WS2812 on GPIO40, colour order GRB, driven by the RMT
  peripheral via `espressif/led_strip`. `bsp_led_set()` scales every channel to at most
  40/255 because full power is glaring; the demo's LED button cycles
  Off/Red/Green/Blue/White.
- microSD: `bsp_sdcard.c` mounts FAT at `/sdcard` over 4-bit SDMMC — CLK 5, CMD 4,
  D0-D3 = 6/7/2/3 (the vendor's own SD example uses the same pins). The lines have
  external pull-ups. There's no card-detect pin, so a card is only picked up at boot, and
  the mount never formats a card. The demo's status bar shows free space, or "none".
  Cards must use an **MBR** partition table with FAT/FAT32: IDF's FatFs is built without
  GPT support (`FF_LBA64 0`), so a GPT card mounts as "no filesystem". On a Mac:
  `diskutil eraseDisk FAT32 ESP32 MBRFormat /dev/diskN`.

## Wi-Fi and clock

- In the demo, tap the Wi-Fi label in the top-right of the home screen to open setup: it scans,
  lists nearby networks (strongest first), and takes the password on an on-screen
  keyboard. WPA/WPA2/WPA3-Personal and open networks are supported; enterprise and WEP
  networks are listed but disabled.
- `net/wifi.c` saves the network in NVS (namespace `wifi`) only after it connects, so a
  mistyped password never replaces a working network — after three rejected attempts
  the screen shows why and the board falls back to the saved network. A saved network
  that's unreachable is retried with backoff (2s up to 30s). "Forget" erases it.
- `net/clock.c` syncs time from `pool.ntp.org` each time Wi-Fi gets an address and shows
  it in the status bar. Apps pass the timezone as a POSIX TZ string to
  `clock_init()`; the demo uses US Pacific.
- Memory: Wi-Fi needs internal RAM next to LVGL's three DMA draw buffers, so the buffers
  are 32 lines each and the Wi-Fi driver's IRAM speed options are off
  (`common/sdkconfig.defaults`); Wi-Fi/lwIP buffers prefer PSRAM. About 87KB of internal RAM is
  free after boot (logged by `main`). The app partition is 4MB (`common/partitions.csv`).
