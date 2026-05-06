# e-ink sugar

Personal blood-sugar (CGM) dashboard for an Elecrow 5.79" e-paper panel driven by an ESP32-S3.

The device wakes on the CGM's 5-minute publish rhythm, fetches the latest readings from a Nightscout-style endpoint, renders one frame to the e-paper, then deep-sleeps until ~10 seconds after the next expected reading.

## What's on screen

- Clock (`17:20`) and date (`5th May`) at top-left
- Big bold current BG reading with a strikethrough when the latest data is older than 15 minutes
- Signed delta vs the previous reading and minutes-since-last-reading (`-1.8`, `5 min`)
- Trend arrow (Flat / FortyFiveUp / DoubleDown / etc., as Nightscout direction strings) drawn as crisp vector polylines
- Time-aware scatter chart of the last 3 hours, plotting each reading at its actual timestamp — CGM dropouts show as genuine spatial gaps rather than a misleading line
- Auto-scaled Y axis (top = `max(reading + 1, 10)` mmol/L) with dashed threshold lines at 4 and 10 mmol/L

The right half of the screen is reserved for an upcoming calendar/appointments phase.

## Hardware

- **Elecrow CrowPanel ESP32-S3 5.79" e-paper HMI display** — 272×792 dual-SSD1683 panel (`GxEPD2_579_GDEY0579T93`), driven landscape (792×272 pixels)
- ESP32-S3 with PSRAM, 8 MB flash

[Product page](https://www.elecrow.com/crowpanel-esp32-5-79-e-paper-hmi-display-with-272-792-resolution-black-white-color-driven-by-spi-interface.html)

## Software

- Arduino framework via PlatformIO
- LVGL 9 for the UI (hand-coded landscape layout, no SquareLine workflow)
- GxEPD2 driving the e-paper
- Custom-generated Montserrat-Bold 72 px font for the BG number (regenerated with `lv_font_conv` if you want a different size)
- Plain TSV parsing (no JSON dep) for the Nightscout `entries.txt` endpoint
- Adaptive deep-sleep cadence so wakes line up with the CGM's publish window

## Build

PlatformIO project (`platformio.ini`); no Arduino IDE workflow.

```
pio run               # build
pio run -t upload     # flash
pio device monitor    # serial @ 115200
```

## Configuration

Edit the placeholders at the top of `src/main.cpp` before flashing:

- `SSID_NAME` / `SSID_PASSWORD`
- `BG_API_URL` — defaults to a personal Nightscout-style endpoint, change to your own (`https://<your-host>/api/v1/entries.txt?count=30`)
- `BUTTON_PIN` — used for an on-demand layout-preview mode that renders synthetic worst-case readings; set to whichever GPIO your hardware exposes as a side button

## License

MIT — see [LICENSE](LICENSE).
