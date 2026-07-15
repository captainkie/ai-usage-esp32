# Hardware — Waveshare ESP32-S3-Touch-LCD-3.49

Reference notes for the board this project targets. Used to build the README and
to pin the display/touch driver choices for the firmware.

- Product page: <https://www.waveshare.com/esp32-s3-touch-lcd-3.49.htm>
- Wiki / docs: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49> · <https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49>

## At a glance

| | |
|---|---|
| **MCU** | ESP32-S3R8 — Xtensa 32-bit LX7 dual-core, up to 240 MHz |
| **Memory** | 512 KB SRAM · 384 KB ROM · **8 MB PSRAM** · **16 MB Flash** |
| **Display** | 3.49″ IPS, **640 × 172** px, 16.7M colours, wide "bar" aspect (~3.7:1) |
| **Display + touch controller** | **AXS15231B** (single chip: LCD driver + capacitive touch) |
| **Display bus** | **QSPI** (Quad-SPI) |
| **Touch bus** | **I²C** (capacitive, multi-touch) |
| **Wireless** | 2.4 GHz Wi-Fi 802.11 b/g/n · Bluetooth 5 (LE), onboard antenna |
| **Sensors** | QMI8658 6-axis IMU (3-axis accel + 3-axis gyro) |
| **Audio** | Dual-microphone array (noise reduction + echo cancellation) |
| **Frameworks** | Arduino IDE · ESP-IDF · LVGL v8 / v9 |

## Why these driver choices

The display is driven over **QSPI** by an **AXS15231B**. For this exact panel the
proven, Waveshare-demoed path on Arduino is the **`Arduino_GFX` (GFX Library for
Arduino)** stack with its dedicated `Arduino_AXS15231B` driver over
`Arduino_ESP32QSPI`. LovyanGFX's QSPI support for AXS15231B is newer and less
battle-tested for this board, so we default to `Arduino_GFX`.

- Full-frame rendering: allocate a `640 × 172 × 2 B ≈ 220 KB` canvas in the 8 MB
  PSRAM and blit once per frame → flicker-free updates on the arc gauges + mascot.
- Touch: AXS15231B touch is read over I²C; used for switching provider and
  companion (cat / robot) on-screen.

## Orientation

The panel is physically **172 (H) × 640 (W)** and we use it in **landscape**
(640 wide × 172 tall) — a long bar, ideal for the twin-gauge dashboard. Set the
`Arduino_GFX` rotation so the long edge is horizontal.

## Physical I/O & enclosure

From the Waveshare board/case photos (`esp32/picture/`):

- **USB-C** (power + flashing/serial), **microSD (TF) slot** (assets / config / logo cache).
- **Li-po battery connector** — the board can run untethered → a genuinely portable
  desk gadget. (There's a `BAT` pad and an onboard battery connector.)
- **Broken-out GPIO headers**: `BAT 3V3 G SCL SDA 44 43 41 40 39 38` (top) and
  `5V 3V3 G 20 19 5 4 3 2 1 0` (bottom) — `SCL/SDA` are the shared I²C bus (touch + IMU).
- **Dual microphones** (MIC1 / MIC2) and **QMI8658 6-axis IMU** onboard.
- **Optional enclosure** (blue case): 3 side buttons — **reset**, **power/boot**, and a
  **custom (gear) button** — plus a speaker grille, power LED, and a fold-out stand.
  The custom button + touch give us physical ways to cycle **provider** and **companion
  (cat / robot)** without the phone.

**Dimensions (cased):** outer `98.50 × 34.80 × 31.80 mm` (stand folded `17.30 mm`
thick); display active area `84.00 × 22.58 mm`, visible glass `92.69 × 28.99 mm`.

**Reference images** (in `esp32/picture/`): `…details-intro` (labelled PCB),
`…details-size1/2` (dimensions), `Arduino-IDE` / `ESP-IDF` (toolchain), plus several
`…details-N` product shots — reusable in the README.

## Design → hardware mapping

The screen preview (`design/mockup.html`) is authored in the exact **640 × 172**
coordinate space, so it ports directly:

- Twin 270° arc gauges → drawn with `Arduino_GFX` arcs/`fillArc` on the PSRAM canvas.
- Traffic-light colours (green `<60%` · amber `60–85%` · red `>85%`) → same thresholds.
- Pixel companion (cat / robot) → a small sprite grid blitted per frame; fur colour =
  active provider, expression = busiest gauge (chill → focus → sweat → stress → fried).
- Touch pills + optional side buttons → switch provider / companion.

## Relevant libraries (Arduino)

- `GFX Library for Arduino` (moononournation/Arduino_GFX) — provides
  `Arduino_AXS15231B` + `Arduino_ESP32QSPI`.
- Touch: AXS15231B touch over I²C (Waveshare demo driver / `bb_captouch`-style).
- `WiFiManager` — captive-portal Wi-Fi + bridge-IP provisioning (no hard-coded
  credentials; friendly for a giveaway).
- `ArduinoJson` — parse the usage JSON from the Mac bridge.

## Notes / gotchas seen in the wild

- QSPI AXS15231B panels are sensitive to init sequence + bus config; if the
  screen shows static/noise, the QSPI init sequence or CS/clock config is usually
  the culprit (documented across several AXS15231B boards).
- 8 MB PSRAM is plenty for a full framebuffer; enable PSRAM in the build config
  and place the canvas there rather than internal SRAM.

_Last verified: 2026-07-15 (specs from Waveshare product page + wiki)._
