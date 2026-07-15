# Firmware — AI Usage Bar (ESP32-S3-Touch-LCD-3.49)

Renders the dashboard on the 640×172 AXS15231B panel: twin 270° arc gauges,
model + effort, live reset countdown, touch-switchable providers, and a pixel
companion. It polls the [Mac bridge](../bridge) over your LAN.

This board's panel + capacitive touch + IO expander are non-trivial to bring up,
so this firmware **stands on Waveshare's own working example** rather than
re-implementing the driver. You drop these files into a copy of their
`09_LVGL_V8_Test` sketch.

## What you need

- **Arduino IDE** with **ESP32 by Espressif** boards (Arduino-ESP32 **3.x**).
- Waveshare demo download → **`Examples/Arduino/09_LVGL_V8_Test`** and the
  bundled `Arduino_Libraries` (from
  [github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49)).
- Libraries (Library Manager): **lvgl 8.3.x**, **WiFiManager** (tzapu),
  **ArduinoJson 6.x**. Plus Waveshare's bundled `SensorLib`.
- The bridge running on your Mac (`../bridge`), and its **IP:port**.

## Assemble the sketch

1. Copy Waveshare's `09_LVGL_V8_Test` folder somewhere and **rename it
   `ai-usage-esp32`** (Arduino needs the folder name to match the `.ino`).
2. **Delete** `09_LVGL_V8_Test.ino` from it, and **copy in** every file from
   this `firmware/ai-usage-esp32/` folder:
   `ai-usage-esp32.ino`, `config.h`, `mascot.h`, `mascot.c`, `net.h`.
   Keep all of Waveshare's other files (`user_config.h`, `lvgl_port.*`,
   `i2c_bsp.*`, `src/…`).
3. **Rotate to landscape** — in `user_config.h` set:
   ```c
   #define Rotated USER_DISP_ROT_90   // 640×172 landscape
   ```
4. **Expose the LVGL lock** — this sketch builds its UI from `setup()`, which
   must hold LVGL's mutex. Waveshare keeps it `static`, so add two thin wrappers.

   In **`lvgl_port.c`**, *after* the `example_lvgl_unlock()` definition:
   ```c
   /* --- AI Usage Bar: expose the LVGL lock --- */
   bool aiusage_lvgl_lock(int timeout_ms) { return example_lvgl_lock(timeout_ms); }
   void aiusage_lvgl_unlock(void)         { example_lvgl_unlock(); }
   ```
   In **`lvgl_port.h`**, inside the `extern "C"` block:
   ```c
   bool aiusage_lvgl_lock(int timeout_ms);
   void aiusage_lvgl_unlock(void);
   ```
5. **Enable the fonts** used by the UI — in LVGL's `lv_conf.h`:
   ```c
   #define LV_FONT_MONTSERRAT_14 1
   #define LV_FONT_MONTSERRAT_20 1
   #define LV_FONT_MONTSERRAT_40 1
   ```
   (Also make sure `LV_COLOR_DEPTH 16` — the default for this board.)

## Board settings (Tools ▸)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| Flash Size | **16MB (128Mb)** |
| PSRAM | **OPI PSRAM** |
| Partition Scheme | **16M Flash (3MB APP / 9.9MB FATFS)** or **Huge APP** |
| USB CDC On Boot | **Enabled** (for Serial) |
| CPU Frequency | 240 MHz |

## Flash & set up

1. Plug in via **USB-C**, select the port, **Upload**.
2. On first boot the screen’s Wi-Fi isn’t configured → the board starts a
   captive-portal AP **`AI-Usage-Bar-Setup`**. Join it from your phone/Mac,
   pick your Wi-Fi, and enter the **Mac bridge IP** and **port** (default
   `8787`) in the two extra fields. Save.
3. It reconnects and starts polling `http://<mac-ip>:8787/usage`. Done.

To change the bridge/Wi-Fi later, re-open the portal (erase Wi-Fi or add a
button that calls `net_portal()`).

## Using it

- **Tap a provider pill** (bottom) to switch Claude / Codex / Gemini — gauges,
  colours, and the companion recolour to match.
- **Tap the companion** to cycle 🐱 → 🤖 → 🐺 → 🦉 → 💀 → 🏃 → 🦖.
- Gauge colour is traffic-light: green `<60%`, amber `60–85%`, red `>85%`. The
  companion's mood tracks the busiest gauge (chill → focus → sweat → stress →
  fried).

## How it fits together

| File | Role |
|---|---|
| `ai-usage-esp32.ino` | LVGL UI (arcs, labels, pills, mascot canvas), render timer, net loop |
| `mascot.c` / `.h` | pixel-companion engine (RGB565), ported 1:1 from `design/mockup.html` |
| `net.h` | WiFiManager captive portal + `GET /usage` + JSON parse |
| `config.h` | thresholds, palette, poll interval, shared `UsageState` |

The mascot engine is pure C and is verified on a host against the web mockup
(`design/tools/mascot_ctest.c` → `design/previews/mascot-moods.png`), so the
device draws exactly what the preview shows.

## Notes

- **Model shows a box instead of `·`?** LVGL's Montserrat may not include the
  middot. Either enable it in `lv_conf.h`, or change the separator in the
  bridge's `prettyModel()` from `·` to `-`.
- **V2 boards** swapped `LCD_TE`/`LCD_RESET`; use the `user_config.h` that ships
  with your board revision.
- Not compiled on-device by the author (no board on hand) — expect to tweak font
  sizes / a few pixel offsets to taste on first flash. The bridge and the mascot
  engine are both verified on a host.
