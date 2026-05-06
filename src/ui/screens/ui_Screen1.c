// Hand-written landscape screen. Left zone is the BG dashboard; right zone
// is the 3-day calendar agenda.

#include <string.h>
#include <stdio.h>
#include "../ui.h"

// ---- Layout constants -------------------------------------------------
#define SCREEN_W           792
#define SCREEN_H           272

#define LEFT_W             450  // BG shifted left + delta/arrow nudged right

#define HEADER_H           80

#define CHART_X            10
#define CHART_Y            (HEADER_H + 4)
#define CHART_W            370   // 20px wider than the original layout
#define CHART_H            (SCREEN_H - CHART_Y - 6)

#define AXIS_X             (CHART_X + CHART_W + 6)

#define CHART_WINDOW_S     (3 * 3600)   // chart spans the last 3 hours

#define Y_MIN_DG           0
#define Y_MIN_TOP_DG       100   // axis top is at least 10 mmol/L
#define LOW_THRESHOLD_DG   40    // 4.0 mmol/L
#define HIGH_THRESHOLD_DG  100   // 10.0 mmol/L

#define ARROW_BOX          40

// ---- Right-zone (calendar agenda) -------------------------------------
#define AGENDA_X           417
#define AGENDA_W           (SCREEN_W - AGENDA_X)   // 367
#define AGENDA_COLS        2
#define AGENDA_COL_GAP     8
#define AGENDA_COL_W       ((AGENDA_W - (AGENDA_COLS + 1) * AGENDA_COL_GAP) / AGENDA_COLS)  // ~175
#define AGENDA_HEADER_H    50

// ---- Globals ----------------------------------------------------------
lv_obj_t * ui_Screen1;

lv_obj_t * ui_time;
lv_obj_t * ui_date;
lv_obj_t * ui_time_since;

lv_obj_t * ui_bg_value;
lv_obj_t * ui_bg_delta;
lv_obj_t * ui_arrow_line1;   // hidden in this layout; fixed polylines are
lv_obj_t * ui_arrow_line2;   // ready for re-enabling once a placement is decided

lv_obj_t * ui_chart;
static lv_chart_series_t * ui_chart_series;

// Agenda widgets. Header/subtitle are pre-allocated; events are children of
// a per-column flex-column container so multi-line wrapped titles push the
// next event downward instead of overlapping.
static lv_obj_t * agenda_day_header[AGENDA_COLS];
static lv_obj_t * agenda_day_isla[AGENDA_COLS];
static lv_obj_t * agenda_day_sep[AGENDA_COLS];
static lv_obj_t * agenda_events_box[AGENDA_COLS];

// Threshold lines + dynamic axis state — references kept so the Y-axis
// can be re-scaled as data changes between wakes.
static lv_obj_t * threshold_line_low;
static lv_obj_t * threshold_line_high;
#define MAX_AXIS_LABELS 8
static lv_obj_t * axis_labels[MAX_AXIS_LABELS];
static int        axis_label_values[MAX_AXIS_LABELS];
static int        axis_label_count = 0;
static int32_t    current_y_max_dg = 220;

// ---- Trend arrow polylines (in a 38x40 box, X compressed from 40 grid)
// Single arrows: vertical shaft + V head, traced as one continuous path.
static const lv_point_precise_t arr_flat[5]   = {{3,20}, {35,20}, {29,14}, {35,20}, {29,26}};
static const lv_point_precise_t arr_up[5]     = {{19,37},{19,3},  {13,10}, {19,3},  {25,10}};
static const lv_point_precise_t arr_down[5]   = {{19,3}, {19,37}, {13,30}, {19,37}, {25,30}};
static const lv_point_precise_t arr_45up[5]   = {{6,34}, {32,6},  {25,8},  {32,6},  {30,14}};
static const lv_point_precise_t arr_45down[5] = {{6,6},  {32,34}, {25,32}, {32,34}, {30,26}};

// Doubles: two clearly separated parallel arrows (no shared wing endpoints).
static const lv_point_precise_t arr_2up_a[5]  = {{11,37},{11,3},  {6,10},  {11,3},  {17,10}};
static const lv_point_precise_t arr_2up_b[5]  = {{27,37},{27,3},  {21,10}, {27,3},  {32,10}};
static const lv_point_precise_t arr_2dn_a[5]  = {{11,3}, {11,37}, {6,30},  {11,37}, {17,30}};
static const lv_point_precise_t arr_2dn_b[5]  = {{27,3}, {27,37}, {21,30}, {27,37}, {32,30}};

void ui_set_trend(const char * direction)
{
    if(ui_arrow_line1 == NULL) return;
    lv_obj_add_flag(ui_arrow_line2, LV_OBJ_FLAG_HIDDEN);
    if(direction == NULL) {
        lv_obj_add_flag(ui_arrow_line1, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(ui_arrow_line1, LV_OBJ_FLAG_HIDDEN);

    if(strcmp(direction, "Flat") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_flat, 5);
    } else if(strcmp(direction, "SingleUp") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_up, 5);
    } else if(strcmp(direction, "FortyFiveUp") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_45up, 5);
    } else if(strcmp(direction, "SingleDown") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_down, 5);
    } else if(strcmp(direction, "FortyFiveDown") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_45down, 5);
    } else if(strcmp(direction, "DoubleUp") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_2up_a, 5);
        lv_line_set_points(ui_arrow_line2, arr_2up_b, 5);
        lv_obj_remove_flag(ui_arrow_line2, LV_OBJ_FLAG_HIDDEN);
    } else if(strcmp(direction, "DoubleDown") == 0) {
        lv_line_set_points(ui_arrow_line1, arr_2dn_a, 5);
        lv_line_set_points(ui_arrow_line2, arr_2dn_b, 5);
        lv_obj_remove_flag(ui_arrow_line2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_arrow_line1, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- Strikethrough (used when the latest reading is older than 15 min)
void ui_set_bg_stale(bool stale)
{
    lv_obj_set_style_text_decor(
        ui_bg_value,
        stale ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE,
        LV_PART_MAIN);
}

// ---- Y-axis dynamic positioning helpers -------------------------------
static int y_for_dg(int32_t mmol_dg, int32_t y_max_dg)
{
    return CHART_Y + (y_max_dg - mmol_dg) * CHART_H / (y_max_dg - Y_MIN_DG);
}

static void update_threshold_position(lv_obj_t * line, int mmol_dg, int32_t y_max_dg)
{
    if(mmol_dg > y_max_dg) {
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    int y = y_for_dg(mmol_dg, y_max_dg);
    lv_point_precise_t * pts = lv_malloc(sizeof(lv_point_precise_t) * 2);
    pts[0].x = CHART_X;            pts[0].y = y;
    pts[1].x = CHART_X + CHART_W;  pts[1].y = y;
    lv_line_set_points(line, pts, 2);
}

static void update_axis_labels(int32_t y_max_dg)
{
    for(int i = 0; i < axis_label_count; i++) {
        int v = axis_label_values[i];
        if(v * 10 > y_max_dg || v * 10 < Y_MIN_DG) {
            lv_obj_add_flag(axis_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(axis_labels[i], LV_OBJ_FLAG_HIDDEN);
        int y = y_for_dg(v * 10, y_max_dg);
        lv_obj_set_pos(axis_labels[i], AXIS_X, y - 8);
    }
}

// ---- Chart helpers ----------------------------------------------------
// Y axis auto-scales: top is max(highest_data_value + 1 mmol/L, 10 mmol/L),
// rounded up to a whole mmol/L. Bottom is 0. Out-of-window points are
// dropped (not clipped) so dropouts show as genuine spatial gaps.
void ui_chart_set_points(const int32_t * dg, const long long * mills,
                         uint16_t count, long long now_mills)
{
    int32_t  x_in[40];
    int32_t  y_in[40];
    uint16_t kept = 0;
    int32_t  max_y_dg = 0;
    for(uint16_t i = 0; i < count && kept < 40; i++) {
        int32_t rel_s = (int32_t)((mills[i] - now_mills) / 1000);
        if(rel_s < -CHART_WINDOW_S || rel_s > 0) continue;
        x_in[kept] = rel_s;
        y_in[kept] = dg[i];
        if(dg[i] > max_y_dg) max_y_dg = dg[i];
        kept++;
    }

    // Y range: ceil(max_data + 1 mmol/L) up to nearest whole mmol, with a
    // floor of 10 mmol/L so the upper threshold line is always visible.
    int32_t y_max_dg = max_y_dg + 10;                  // +1 mmol/L
    y_max_dg = ((y_max_dg + 9) / 10) * 10;             // ceil to whole mmol
    if(y_max_dg < Y_MIN_TOP_DG) y_max_dg = Y_MIN_TOP_DG;
    current_y_max_dg = y_max_dg;

    lv_chart_set_range(ui_chart, LV_CHART_AXIS_PRIMARY_Y, Y_MIN_DG, y_max_dg);
    update_threshold_position(threshold_line_low,  LOW_THRESHOLD_DG,  y_max_dg);
    update_threshold_position(threshold_line_high, HIGH_THRESHOLD_DG, y_max_dg);
    update_axis_labels(y_max_dg);

    if(kept == 0) {
        lv_chart_set_point_count(ui_chart, 0);
        return;
    }

    lv_chart_set_point_count(ui_chart, kept);
    for(uint16_t i = 0; i < kept; i++) {
        lv_chart_set_series_value_by_id2(ui_chart, ui_chart_series, i, x_in[i], y_in[i]);
    }
    lv_chart_refresh(ui_chart);
}

// ---- Screen builder ---------------------------------------------------
static lv_obj_t * make_threshold_line(int dash_w)
{
    // Initial points are placeholders; update_threshold_position() resets
    // them to the correct y for the current chart range.
    lv_point_precise_t * pts = lv_malloc(sizeof(lv_point_precise_t) * 2);
    pts[0].x = CHART_X;            pts[0].y = CHART_Y + CHART_H;
    pts[1].x = CHART_X + CHART_W;  pts[1].y = CHART_Y + CHART_H;

    lv_obj_t * line = lv_line_create(ui_Screen1);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_width(line, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(line, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_line_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(line, dash_w, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(line, dash_w, LV_PART_MAIN);
    return line;
}

static void register_axis_label(int mmol)
{
    if(axis_label_count >= MAX_AXIS_LABELS) return;
    lv_obj_t * lbl = lv_label_create(ui_Screen1);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d", mmol);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
    axis_labels[axis_label_count]       = lbl;
    axis_label_values[axis_label_count] = mmol;
    axis_label_count++;
}

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_Screen1, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Screen1, 0, LV_PART_MAIN);

    // ---- Clock + date (top-left) -------------------------------------
    // Mont 40 clock + Mont 20 date stacks to ~72px tall, matching the
    // BG label's Mont-Bold-72 line height, so all three labels share a
    // bottom edge and the header reads as one row.
    ui_time = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_time, "--:--");
    lv_obj_set_style_text_font(ui_time, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_time, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(ui_time, 12, 0);

    ui_date = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_date, "");
    lv_obj_set_style_text_font(ui_date, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_date, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_pos(ui_date, 14, 46);

    // ---- BG value (huge, custom-generated Montserrat-Bold 72) --------
    // Centered inside a 165px slot so single-digit ("8.8") readings sit in
    // the same visual position as worst-case ("16.6") ones.
    ui_bg_value = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_bg_value, "--.-");
    lv_obj_set_style_text_font(ui_bg_value, &lv_font_montserrat_bold_72, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_bg_value, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_bg_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(ui_bg_value, 165);
    lv_obj_set_pos(ui_bg_value, 110, 0);

    // ---- Trend arrow (rightmost, after the delta stack) --------------
    int arrow_x = 361;   // shifted 2px right
    int arrow_y = 17;
    int arrow_w = 38;    // 2px narrower than the height
    int arrow_h = ARROW_BOX;

    ui_arrow_line1 = lv_line_create(ui_Screen1);
    lv_line_set_points(ui_arrow_line1, arr_flat, 5);
    lv_obj_set_size(ui_arrow_line1, arrow_w, arrow_h);
    lv_obj_set_pos(ui_arrow_line1, arrow_x, arrow_y);
    lv_obj_set_style_line_width(ui_arrow_line1, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(ui_arrow_line1, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_arrow_line1, true, LV_PART_MAIN);

    ui_arrow_line2 = lv_line_create(ui_Screen1);
    lv_line_set_points(ui_arrow_line2, arr_flat, 5);
    lv_obj_set_size(ui_arrow_line2, arrow_w, arrow_h);
    lv_obj_set_pos(ui_arrow_line2, arrow_x, arrow_y);
    lv_obj_set_style_line_width(ui_arrow_line2, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(ui_arrow_line2, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_arrow_line2, true, LV_PART_MAIN);
    lv_obj_add_flag(ui_arrow_line2, LV_OBJ_FLAG_HIDDEN);   // toggled on by DoubleUp/DoubleDown

    // ---- Delta + time-since (right of the arrow, centered as a stack)
    // Wider so "+0.4" / "-0.2" at Mont 40 never wraps; pushed left to
    // compensate. Stack stays right-aligned so the visible delta keeps
    // hugging the arrow column and doesn't overrun the BG label.
    int stack_x = 258;
    int stack_w = 100;

    ui_bg_delta = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_bg_delta, "");
    lv_obj_set_style_text_font(ui_bg_delta, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_bg_delta, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_bg_delta, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(ui_bg_delta, stack_w);
    lv_obj_set_pos(ui_bg_delta, stack_x, 4);

    ui_time_since = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_time_since, "");
    lv_obj_set_style_text_font(ui_time_since, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_time_since, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_time_since, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(ui_time_since, stack_w);
    lv_obj_set_pos(ui_time_since, stack_x, 45);

    // ---- Threshold lines (drawn first → behind the chart) ------------
    threshold_line_low  = make_threshold_line(4);
    threshold_line_high = make_threshold_line(6);

    // ---- Chart -------------------------------------------------------
    ui_chart = lv_chart_create(ui_Screen1);
    lv_obj_set_pos(ui_chart, CHART_X, CHART_Y);
    lv_obj_set_size(ui_chart, CHART_W, CHART_H);
    lv_chart_set_type(ui_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_div_line_count(ui_chart, 0, 0);
    lv_chart_set_range(ui_chart, LV_CHART_AXIS_PRIMARY_Y, Y_MIN_DG, current_y_max_dg);
    lv_chart_set_range(ui_chart, LV_CHART_AXIS_PRIMARY_X, -CHART_WINDOW_S, 0);
    lv_chart_set_point_count(ui_chart, 40);

    lv_obj_set_style_pad_all(ui_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_chart, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_chart, 0, LV_PART_MAIN);

    lv_obj_set_style_line_width(ui_chart, 0, LV_PART_ITEMS);
    lv_obj_set_style_size(ui_chart, 5, 5, LV_PART_INDICATOR);

    ui_chart_series = lv_chart_add_series(ui_chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);

    // Register potential axis labels — update_axis_labels() hides any that
    // exceed the current Y range.
    register_axis_label(2);
    register_axis_label(4);
    register_axis_label(7);
    register_axis_label(10);
    register_axis_label(13);
    register_axis_label(16);
    register_axis_label(19);
    register_axis_label(22);

    // Position thresholds + labels for the initial range.
    update_threshold_position(threshold_line_low,  LOW_THRESHOLD_DG,  current_y_max_dg);
    update_threshold_position(threshold_line_high, HIGH_THRESHOLD_DG, current_y_max_dg);
    update_axis_labels(current_y_max_dg);

    // ---- Right zone — agenda columns --------------------------------
    // Optional: a subtle 1px vertical separator between the dashboard and
    // agenda zones, so the eye registers the split.
    {
        lv_point_precise_t * pts = lv_malloc(sizeof(lv_point_precise_t) * 2);
        pts[0].x = AGENDA_X - 4; pts[0].y = 4;
        pts[1].x = AGENDA_X - 4; pts[1].y = SCREEN_H - 4;
        lv_obj_t * sep = lv_line_create(ui_Screen1);
        lv_line_set_points(sep, pts, 2);
        lv_obj_set_style_line_width(sep, 1, LV_PART_MAIN);
        lv_obj_set_style_line_color(sep, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_line_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_line_dash_width(sep, 2, LV_PART_MAIN);
        lv_obj_set_style_line_dash_gap(sep, 3, LV_PART_MAIN);
    }

    for (int c = 0; c < AGENDA_COLS; c++)
    {
        int col_x = AGENDA_X + AGENDA_COL_GAP + c * (AGENDA_COL_W + AGENDA_COL_GAP);

        agenda_day_header[c] = lv_label_create(ui_Screen1);
        lv_obj_set_pos(agenda_day_header[c], col_x, 2);
        lv_obj_set_width(agenda_day_header[c], AGENDA_COL_W);
        lv_obj_set_style_text_font(agenda_day_header[c], &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(agenda_day_header[c], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(agenda_day_header[c], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(agenda_day_header[c], LV_LABEL_LONG_CLIP);
        lv_label_set_text(agenda_day_header[c], "");

        agenda_day_isla[c] = lv_label_create(ui_Screen1);
        lv_obj_set_pos(agenda_day_isla[c], col_x, 26);
        lv_obj_set_width(agenda_day_isla[c], AGENDA_COL_W);
        lv_obj_set_style_text_font(agenda_day_isla[c], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(agenda_day_isla[c], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(agenda_day_isla[c], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(agenda_day_isla[c], LV_LABEL_LONG_CLIP);
        lv_label_set_text(agenda_day_isla[c], "");

        // Thin separator under the header.
        lv_point_precise_t * sep_pts = lv_malloc(sizeof(lv_point_precise_t) * 2);
        sep_pts[0].x = col_x;                  sep_pts[0].y = AGENDA_HEADER_H - 4;
        sep_pts[1].x = col_x + AGENDA_COL_W;   sep_pts[1].y = AGENDA_HEADER_H - 4;
        agenda_day_sep[c] = lv_line_create(ui_Screen1);
        lv_line_set_points(agenda_day_sep[c], sep_pts, 2);
        lv_obj_set_style_line_width(agenda_day_sep[c], 1, LV_PART_MAIN);
        lv_obj_set_style_line_color(agenda_day_sep[c], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_line_opa(agenda_day_sep[c], LV_OPA_COVER, LV_PART_MAIN);

        // Flex-column container for events. Children stack and a wrapped
        // title pushes the next event downward instead of overlapping.
        // pad_row applies between every sibling (row, sep, row, sep, ...),
        // so the line gets a little vertical breathing room either side.
        lv_obj_t * box = lv_obj_create(ui_Screen1);
        lv_obj_set_pos(box, col_x, AGENDA_HEADER_H);
        lv_obj_set_size(box, AGENDA_COL_W, SCREEN_H - AGENDA_HEADER_H - 2);
        lv_obj_set_layout(box, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        agenda_events_box[c] = box;
    }
}

// ---- Calendar agenda public API --------------------------------------
void ui_calendar_clear_day(int col)
{
    if (col < 0 || col >= AGENDA_COLS) return;
    lv_label_set_text(agenda_day_header[col], "");
    lv_label_set_text(agenda_day_isla[col], "");
    lv_obj_clean(agenda_events_box[col]);
}

void ui_calendar_set_day_header(int col, const char * date_label, const char * isla_with)
{
    if (col < 0 || col >= AGENDA_COLS) return;
    lv_label_set_text(agenda_day_header[col], date_label ? date_label : "");
    if (isla_with && isla_with[0])
    {
        char buf[40];
        lv_snprintf(buf, sizeof(buf), "Isla: %s", isla_with);
        lv_label_set_text(agenda_day_isla[col], buf);
    }
    else
    {
        lv_label_set_text(agenda_day_isla[col], "");
    }
}

// Adds one event row to a column's flex container. Always followed by a
// thin horizontal divider so the eye picks each event out as a unit.
//   - All-day events: bold centered title on a black banner with white text.
//   - Timed events: fixed-width bold "HH:MM" prefix (right-aligned within
//     its slot so 8:14 and 12:30 share an x for the title that follows),
//     plus a regular wrapping title that takes the remaining width.
#define AGENDA_TIME_W 38   // fits "23:59" at Mont Bold 14 with margin

void ui_calendar_add_event(int col, bool all_day, int hh, int mm, const char * title)
{
    if (col < 0 || col >= AGENDA_COLS) return;
    lv_obj_t * box = agenda_events_box[col];

    if (all_day)
    {
        // Bigger weight font (Bold 18) makes the inverted banner read as
        // unambiguously bold — Bold 14 dithered to B/W doesn't visually
        // separate from regular at this size.
        lv_obj_t * banner = lv_label_create(box);
        lv_obj_set_width(banner, AGENDA_COL_W);
        lv_label_set_text(banner, title ? title : "");
        lv_obj_set_style_text_font(banner, &lv_font_montserrat_bold_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(banner, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(banner, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(banner, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_top(banner, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(banner, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_left(banner, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_right(banner, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(banner, 0, LV_PART_MAIN);
        lv_label_set_long_mode(banner, LV_LABEL_LONG_DOT);
    }
    else
    {
        lv_obj_t * row = lv_obj_create(box);
        lv_obj_set_size(row, AGENDA_COL_W, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * time_lbl = lv_label_create(row);
        lv_obj_set_width(time_lbl, AGENDA_TIME_W);
        char tbuf[8];
        lv_snprintf(tbuf, sizeof(tbuf), "%d:%02d", hh, mm);
        lv_label_set_text(time_lbl, tbuf);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_bold_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(time_lbl, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        lv_obj_set_style_pad_all(time_lbl, 0, LV_PART_MAIN);

        lv_obj_t * title_lbl = lv_label_create(row);
        // Explicit width (rather than flex_grow) so LV_LABEL_LONG_DOT has a
        // concrete width to truncate against; flex_grow defers width to
        // layout time which can leave DOT mode no-op'd.
        lv_obj_set_width(title_lbl, AGENDA_COL_W - AGENDA_TIME_W - 6);
        lv_label_set_text(title_lbl, title ? title : "");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_lbl, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(title_lbl, 0, LV_PART_MAIN);
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    }

    // Horizontal divider beneath the event.
    lv_obj_t * sep = lv_obj_create(box);
    lv_obj_set_size(sep, AGENDA_COL_W, 1);
    lv_obj_set_style_bg_color(sep, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
}
