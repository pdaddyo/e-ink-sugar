# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for an Elecrow 5.79" e-paper dashboard (792×272 B/W, dual SSD1683, GDEY0579T93 panel) driven by an ESP32-S3 with PSRAM, repurposed from an upstream news/weather demo into a personal **landscape blood-sugar (CGM) dashboard**. On each wake the device joins WiFi, syncs NTP, fetches Nightscout-style entries, renders one frame via LVGL 9 to the e-paper, and deep-sleeps until ~10 seconds after the next expected CGM reading. `loop()` is intentionally empty — every wake is a fresh `setup()`.

The **left zone** of the screen (x: 0..440 — see `LEFT_W` in `ui_Screen1.c`) is in active use. The right zone (x: 440..792) is reserved for an upcoming calendar/appointments phase.

## Build / flash / monitor

PlatformIO (`platformio.ini`); no Makefile or Arduino IDE workflow.

- Build: `pio run`
- Flash: `pio run -t upload`
- Serial monitor: `pio device monitor` (115200 baud)
- Clean: `pio run -t clean`

**Important**: when you toggle anything in `include/lv_conf.h` (e.g. enabling a new `LV_FONT_MONTSERRAT_*` size), run `pio run -t clean` first. LVGL lives in `.pio/libdeps/` so PIO's incremental tracker doesn't see config changes outside `src/`, and you'll get an `undefined reference` link error otherwise.

No automated tests; the `test/` directory is the empty PlatformIO scaffold.

## Architecture

### Boot-and-sleep flow (`src/main.cpp`)
1. `connect_wifi()` — `WiFiMulti` with `WIFI_TIMEOUT_MS` ceiling (no infinite waits).
2. `sync_ntp_clock()` — `NTPClient` then `settimeofday()` so `time(NULL)` and `localtime()` work for both the date label and time-since math.
3. `fetch_bg()` — `WiFiClientSecure` + `setInsecure()`, hits **`entries.txt`** (Nightscout TSV: `dateString \t mills \t sgv \t direction \t device`, one entry per line, newest first). Streamed line-by-line via `Stream::readStringUntil('\n')` + `String::indexOf('\t')` — no JSON document, no ArduinoJson include in this file. The newest-first response is reversed into oldest-first arrays of `dg_oldest_first[]` (0.1 mmol/L integers) and `mills_oldest_first[]` (epoch ms). `bg.latest_mmol`, `bg.delta_mmol`, and `bg.direction` are set from the first two entries.
4. Pin 7 → HIGH powers the e-paper rail; `epd_setup()` initialises SPI on `SCK=12, MOSI=11, CS=45` and the `GxEPD2_BW<GxEPD2_579_GDEY0579T93>` driver (CS=45, DC=46, RST=47, BUSY=48).
5. LVGL inits with two partial-render buffers sized `(792*272/8)+8` bytes each. `my_disp_flush()` forwards each flush region to `display.drawImage()`; the **`+8` byte offset on the buffer pointer is mandatory** — it skips the LVGL 9 image header so GxEPD2 sees raw bitmap data.
6. `ui_init()` → `render_screen()` populates labels and chart. The render code spins `lv_timer_handler()` until `my_disp_flush()` reports the bottom-right tile flushed (`g_full_flush_done`). That is the canonical "frame done" signal — don't replace it with a fixed delay.
7. `compute_sleep_seconds()` returns the wake interval (see "Adaptive sleep" below); `lv_deinit()`, pin 7 LOW, `esp_deep_sleep_start()`.

`mgdl / 18.0182 = mmol/L`; `sgv` from the Nightscout-style API is mg/dL.

### Adaptive sleep
Wake cadence aligns with the CGM's 5-minute publish rhythm rather than running on a free-running 5-minute timer:

- If the latest reading was less than 5 min ago: sleep `(CGM_PERIOD_S + POST_DATA_DELAY) - secs_since_last`, so we wake ~10 seconds after the next expected publish.
- Otherwise (no fetch, or data already older than 5 min): sleep `SLEEP_DEFAULT_S` (5 min) and retry.
- Clamped to `[SLEEP_MIN_S, SLEEP_DEFAULT_S + POST_DATA_DELAY]` (60s..310s) so the radio always has time to come up and we never accidentally pick a 0/negative value.

The chosen interval prints to serial as `[sleep] N s` for visibility.

### LVGL UI (`src/ui/`)
**Hand-coded — no longer SquareLine-managed.** `src/ui/screens/ui_Screen1.c` owns the screen layout and per-screen globals (`ui_Screen1`, `ui_time`, `ui_date`, `ui_bg_value`, `ui_bg_delta`, `ui_time_since`, `ui_arrow_line1/2`, `ui_chart`). `src/ui/ui.c` only calls `ui_Screen1_screen_init()` and loads it.

The pre-existing SquareLine artefacts that remain in-tree (font/image C arrays under `src/ui/fonts/`, `src/ui/images/`, plus `ui_helpers.*`, `ui_events.h`, `components/ui_comp_hook.c`) are still compiled in via `filelist.txt` so the existing fonts remain available if needed. They're not referenced by the live code. **If you regenerate from SquareLine, don't overwrite `ui_Screen1.c` / `ui.h` / `ui.c`** — those are now hand-authored.

Header layout (left zone, all coordinates in `ui_Screen1.c`):
- Clock `17:20` at x=12, `lv_font_montserrat_48`
- Date `5th May` at x=14, y=54, `lv_font_montserrat_20`
- BG value `8.7` at x=165, custom **`lv_font_montserrat_bold_72`** (see Custom fonts below)
- Delta `+0.6` at x=290, width 80, `lv_font_montserrat_40`, center-aligned
- Time-since `5 min` / `now` / `2h 14m` at x=290, y=52, width 80, `lv_font_montserrat_20`, center-aligned (no "ago" suffix)
- Trend arrow at x=375, y=5, 60×60 box. Two `lv_line` widgets — one for single-direction arrows, one revealed only for DoubleUp/DoubleDown so the two parallel shafts read as two arrows. Each direction has a pre-computed 5-point polyline (shaft + V head as one continuous path).

`ui_set_trend(direction)` in `ui_Screen1.c` switches the polyline based on Nightscout direction strings (`Flat`, `SingleUp`, `FortyFiveUp`, `DoubleUp`, `SingleDown`, `FortyFiveDown`, `DoubleDown`). Single arrows hide `ui_arrow_line2`. Unknown strings hide both lines.

`ui_set_bg_stale(stale)` toggles `LV_TEXT_DECOR_STRIKETHROUGH` on the BG label; main.cpp passes `true` when `now - latest_mills/1000 > STALE_THRESHOLD_S` (15 min).

### Chart (`ui_chart_set_points` in `ui_Screen1.c`)
- Type: **`LV_CHART_TYPE_SCATTER`** with `line_width = 0` on `LV_PART_ITEMS` — dot-only, no connecting line. This means a CGM dropout shows as genuine spatial gaps between dots rather than a misleading line jumping across the gap.
- X axis: time-aware, range `[-CHART_WINDOW_S, 0]` (default 3 hours). Each point's X is `(mills - now_mills) / 1000`. Right edge == "now"; a stale latest reading sits short of the right edge by however many minutes it's lagging.
- Out-of-window points are **dropped**, not clamped — old data can't fake itself in at the left edge.
- Y axis: **auto-scaled**. Top = `ceil(max_data + 1 mmol/L)` rounded up to whole mmol, with a floor of 10 mmol/L (`Y_MIN_TOP_DG`) so the upper threshold line is always visible. Bottom = 0.
- Threshold lines at 4 and 10 mmol/L are dashed `lv_line` widgets created **before** the chart; the chart's main background is `LV_OPA_TRANSP` so dashes show through behind the data.
- Y-axis labels are pre-registered at 2 / 4 / 7 / 10 / 13 / 16 / 19 / 22 mmol/L. Each has its position recomputed on every range change; labels outside the current range hide themselves.
- Both threshold lines and axis labels re-position via `update_threshold_position()` / `update_axis_labels()` in `ui_chart_set_points`. `update_threshold_position()` allocates a new 2-point array per call — heap leak is bounded by the wake cycle (deep sleep resets heap).

### LVGL 9 chart API gotcha
LVGL 9.5 renamed the per-index value setters: it's `lv_chart_set_series_value_by_id` and `lv_chart_set_series_value_by_id2` (the `_2` variant takes both X and Y, required for `SCATTER`). The shorter `lv_chart_set_value_by_id` does not exist; some earlier builds in the project compiled through implicit declaration warnings.

### `lv_conf.h` wiring
`include/lv_conf.h` is fed to LVGL via `-D 'LV_CONF_PATH="..."'` in `platformio.ini` — the **single quotes around the whole token are load-bearing** (without them the shell strips the inner quotes and `#include LV_CONF_PATH` errors with "expects FILENAME or <FILENAME>"). Currently enabled stock Montserrat sizes: 14, 16, 18, 20, 28, 40, 48.

### Custom fonts
`src/ui/fonts/lv_font_montserrat_bold_72.c` is a generated LVGL 4-bpp font containing ASCII (0x20–0x7F) of Montserrat Bold at 72 px. It's not stock — LVGL ships only regular-weight Montserrat up to size 48. Generated with [`lv_font_conv`](https://github.com/lvgl/lv_font_conv):

```
lv_font_conv \
  --font /Users/paulbarrass/SquareLine/examples/Smartwatch/assets/fonts/Montserrat-Bold.ttf \
  --size 72 --bpp 4 --format lvgl --no-compress \
  -r 0x20-0x7F \
  -o src/ui/fonts/lv_font_montserrat_bold_72.c
```

`lv_font_conv` is installed via Volta's npm (`~/.volta/bin/lv_font_conv`). The Montserrat-Bold TTF is vendored at the SquareLine examples path noted above. **After generating, edit the file's first include block — replace the `LV_LVGL_H_INCLUDE_SIMPLE` `#ifdef`/`#else` with a flat `#include "lvgl.h"`** (the `#else` branch's `lvgl/lvgl.h` path doesn't resolve under PlatformIO's LVGL package layout).

To resize: regenerate at the new size, update the `LV_FONT_DECLARE` and `lv_obj_set_style_text_font` references in `src/ui/ui.h` and `src/ui/screens/ui_Screen1.c`, and delete the old `.c` file (no symlink/alias — the generated symbol name embeds the size).

### Dependencies
PlatformIO `lib_deps`: `lvgl/lvgl@^9.2.0`, `zinggjm/GxEPD2@^1.5.9`, `bblanchon/ArduinoJson@^6.19.4`. `lib/NTPClient` is vendored locally (not the registry version). ArduinoJson is currently unused at the source level (the BG fetch is TSV) but kept in `lib_deps` for future endpoints — **if you reintroduce it, it's pinned to v6**: filters for arrays use `filter[0]["field"] = true`, not `filter["field"] = true` (the array-vs-object distinction silently parses to zero entries when wrong).

## Configuration that must be set before flashing

`src/main.cpp` ships with placeholders that will not work for someone else:
- `SSID_NAME` / `SSID_PASSWORD`
- `BG_API_URL` (defaults to a personal Nightscout-style endpoint)

No secrets file or build-flag indirection — these are edited in-source. Don't commit real WiFi credentials.

## Diagnostics already wired into the build

`fetch_bg()` and friends emit serial lines that are useful when debugging on hardware:

- `[wifi] connected to <ssid>, ip=<ip>` / `[wifi] timeout, no AP`
- `[ntp] epoch=<unix-seconds>`
- `[bg] GET -> <http-code>`
- `[bg] parsed entries=<n>`
- `[bg] latest=<mmol> delta=<+/-mmol> dir=<direction> pts=<n>`
- `[sleep] <n> s`

These are intentionally always-on (cost is negligible). Remove them only if a future change makes the device noisy on the wire.
