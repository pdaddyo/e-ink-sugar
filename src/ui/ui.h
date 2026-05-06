// Hand-written UI header (formerly SquareLine-generated).

#ifndef _SUGAR_UI_H
#define _SUGAR_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#include "ui_helpers.h"
#include "ui_events.h"

#include <stdbool.h>

// SCREEN: ui_Screen1
void ui_Screen1_screen_init(void);

extern lv_obj_t * ui_Screen1;

extern lv_obj_t * ui_time;
extern lv_obj_t * ui_date;
extern lv_obj_t * ui_time_since;

extern lv_obj_t * ui_bg_value;
extern lv_obj_t * ui_bg_delta;
extern lv_obj_t * ui_arrow_line1;
extern lv_obj_t * ui_arrow_line2;

extern lv_obj_t * ui_chart;

// Sets the trend arrow polyline based on a Nightscout direction string
// (Flat, SingleUp, FortyFiveUp, DoubleUp, SingleDown, FortyFiveDown, DoubleDown,
// or any unrecognised value to hide the arrow). Currently a no-op visually
// because the arrow widgets are hidden; placement TBD.
void ui_set_trend(const char * direction);

// Strikethroughs the BG value when the latest reading is older than the
// staleness threshold (caller decides; firmware uses 15 min).
void ui_set_bg_stale(bool stale);

// Right-zone calendar agenda. Three columns (col=0 today, 1 tomorrow,
// 2 day-after). `clear_day` wipes the existing rows; `set_day_header`
// fills the day name + per-day Isla subtitle; `add_event` appends a
// single event row (`hh`/`mm` ignored when `all_day` is true).
void ui_calendar_clear_day(int col);
void ui_calendar_set_day_header(int col, const char *date_label, const char *isla_with);
void ui_calendar_add_event(int col, bool all_day, int hh, int mm, const char *title);

// Replaces the chart series with `count` points (oldest first).
// `dg[i]`    is the i-th BG value in 0.1 mmol/L units (e.g. 92 == 9.2 mmol/L).
// `mills[i]` is the i-th sample's epoch-ms timestamp.
// `now_mills` is the current wall-clock epoch-ms — points are positioned on
// the chart's X axis as `(mills - now_mills) / 1000`, so the right edge is
// "now" and any reading delay shows as a gap on the right. Y axis auto-scales
// to fit the data with a minimum top of 10 mmol/L.
void ui_chart_set_points(const int32_t * dg, const long long * mills,
                         uint16_t count, long long now_mills);

// FONTS bundled in src/ui/fonts.
LV_FONT_DECLARE(ui_font_Font1);
LV_FONT_DECLARE(ui_font_Font14);
LV_FONT_DECLARE(ui_font_Font20);
LV_FONT_DECLARE(ui_font_Font60);
LV_FONT_DECLARE(ui_font_opensans18);
LV_FONT_DECLARE(ui_font_opensans181);
LV_FONT_DECLARE(ui_font_opensans182);
LV_FONT_DECLARE(ui_font_opensans20);
LV_FONT_DECLARE(ui_font_opensansItalic);
LV_FONT_DECLARE(ui_font_opensansItalic1);
LV_FONT_DECLARE(ui_font_opensansItalic2);
LV_FONT_DECLARE(ui_font_opensansSemiBolt20);
LV_FONT_DECLARE(ui_font_opensansSemiBolt21);
LV_FONT_DECLARE(lv_font_montserrat_bold_72);
LV_FONT_DECLARE(lv_font_montserrat_bold_14);

void ui_init(void);

#ifdef __cplusplus
}
#endif

#endif
