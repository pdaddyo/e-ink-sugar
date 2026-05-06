#include "setup_mode.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <qrcode.h>

#include "lvgl.h"
#include "ui/ui.h"
#include "config.h"

// AP credentials. Static — these are the device's "factory" setup
// network, not the user's Wi-Fi (which is what they're configuring).
#define AP_SSID            "esugar-setup"
#define AP_PASSWORD        "pleaseconfigure"
#define HTTP_PORT          80
#define SETUP_TIMEOUT_MS   (10UL * 60UL * 1000UL)   // 10 min idle → sleep
#define WIFI_QR_PAYLOAD    "WIFI:T:WPA;S:" AP_SSID ";P:" AP_PASSWORD ";;"
#define URL_QR_PAYLOAD     "http://192.168.4.1/"

// QR rendering: Version 4 = 33x33 modules; 6px per module = 198x198px.
#define QR_VERSION         4
#define QR_MODULE_PX       6
#define QR_TOTAL_PX        (33 * QR_MODULE_PX)   // 198

// ---- Provided by main.cpp --------------------------------------------
extern void epd_render_and_refresh(void);
extern void epd_power_off(void);
extern uint64_t button_wakeup_mask(void);

// ---- HTTP server state -----------------------------------------------
static WebServer server(HTTP_PORT);
static volatile bool exit_requested = false;
static volatile bool save_then_reboot = false;

// ---- LVGL screen for setup mode --------------------------------------
static lv_obj_t * setup_screen;

// Render one QR code as an LVGL canvas at (x, y). The canvas buffer is
// allocated through LVGL's allocator and lives for the rest of the
// session — fine because setup mode either reboots or sleeps when done.
static void make_qr_canvas(lv_obj_t * parent, int x, int y, const char * payload)
{
    QRCode qr;
    static uint8_t qr_buf_wifi[200];   // ~144 bytes for v4 ECC_LOW
    static uint8_t qr_buf_url[200];
    // Pick a static buffer per call site so two QR generators don't trample
    // each other if the second qrcode_initText runs before the first is
    // drawn — payload pointer is the disambiguator.
    static const char *seen[2] = {NULL, NULL};
    uint8_t *buf = qr_buf_wifi;
    if (seen[0] == NULL) seen[0] = payload;
    else if (seen[0] != payload) buf = qr_buf_url;
    qrcode_initText(&qr, buf, QR_VERSION, ECC_LOW, payload);

    int total = qr.size * QR_MODULE_PX;

    size_t cbuf_size = LV_CANVAS_BUF_SIZE(total, total, 1, LV_DRAW_BUF_STRIDE_ALIGN);
    uint8_t * cbuf = (uint8_t *)lv_malloc(cbuf_size);
    lv_obj_t * canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, cbuf, total, total, LV_COLOR_FORMAT_I1);
    lv_color32_t white32 = { .blue = 0xFF, .green = 0xFF, .red = 0xFF, .alpha = 0xFF };
    lv_color32_t black32 = { .blue = 0x00, .green = 0x00, .red = 0x00, .alpha = 0xFF };
    lv_canvas_set_palette(canvas, 0, white32);
    lv_canvas_set_palette(canvas, 1, black32);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_obj_set_pos(canvas, x, y);

    for (uint8_t my = 0; my < qr.size; my++) {
        for (uint8_t mx = 0; mx < qr.size; mx++) {
            if (!qrcode_getModule(&qr, mx, my)) continue;
            int px = mx * QR_MODULE_PX;
            int py = my * QR_MODULE_PX;
            for (int dy = 0; dy < QR_MODULE_PX; dy++) {
                for (int dx = 0; dx < QR_MODULE_PX; dx++) {
                    lv_canvas_set_px(canvas, px + dx, py + dy,
                                     lv_color_black(), LV_OPA_COVER);
                }
            }
        }
    }
}

static void build_setup_screen(void)
{
    setup_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(setup_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(setup_screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(setup_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(setup_screen, 0, LV_PART_MAIN);

    // Title bar across the top.
    lv_obj_t * title = lv_label_create(setup_screen);
    lv_label_set_text(title, "Setup mode — connect, then configure");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(title, 792);
    lv_obj_set_pos(title, 0, 4);

    // Two QRs side-by-side: Wi-Fi join on the left, URL on the right.
    int qr_y = 30;
    int qr1_x = 92;
    int qr2_x = 502;
    make_qr_canvas(setup_screen, qr1_x, qr_y, WIFI_QR_PAYLOAD);
    make_qr_canvas(setup_screen, qr2_x, qr_y, URL_QR_PAYLOAD);

    // Captions under each QR (2 lines each, Mont 14, centered).
    int cap_y = qr_y + QR_TOTAL_PX + 4;

    lv_obj_t * cap1 = lv_label_create(setup_screen);
    lv_label_set_text(cap1,
                      "Wi-Fi: " AP_SSID "\n"
                      "Password: " AP_PASSWORD);
    lv_obj_set_style_text_font(cap1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap1, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(cap1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(cap1, QR_TOTAL_PX);
    lv_obj_set_pos(cap1, qr1_x, cap_y);

    lv_obj_t * cap2 = lv_label_create(setup_screen);
    lv_label_set_text(cap2,
                      "Then visit:\n"
                      URL_QR_PAYLOAD);
    lv_obj_set_style_text_font(cap2, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(cap2, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(cap2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(cap2, QR_TOTAL_PX);
    lv_obj_set_pos(cap2, qr2_x, cap_y);

    lv_screen_load(setup_screen);
}

// ---- HTML form -------------------------------------------------------
static String html_escape(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

static String render_form_html(const char *flash_msg = nullptr)
{
    String html;
    html.reserve(3500);
    html += F(R"HTML(<!DOCTYPE html>
<html><head><meta name="viewport" content="width=device-width">
<title>e-ink-sugar setup</title>
<style>
body{font:16px sans-serif;margin:1em;max-width:560px;color:#222}
h1{font-size:1.4em;margin-top:0}
label{display:block;margin:1em 0 .25em;font-weight:bold}
input{width:100%;padding:.5em;box-sizing:border-box;font-family:monospace;font-size:.9em}
.btn-row{margin-top:1.5em;display:flex;gap:.5em}
button{flex:1;padding:.7em;font-size:1em;border:0;cursor:pointer;border-radius:4px}
.save{background:#2a5ade;color:#fff}
.cancel{background:#ccc;color:#222}
.note{color:#666;font-size:.85em;margin:.5em 0}
.flash{background:#dff0d8;border:1px solid #b3d9b0;padding:.5em;border-radius:4px;margin:1em 0}
</style></head><body>
<h1>e-ink-sugar setup</h1>
)HTML");

    if (flash_msg) {
        html += "<div class=\"flash\">";
        html += html_escape(flash_msg);
        html += "</div>";
    }

    html += F("<form method=\"POST\" action=\"/save\">");

    auto field = [&](const char *label, const char *name, const String &value, bool is_password) {
        html += "<label>";
        html += label;
        html += "</label><input name=\"";
        html += name;
        html += "\" type=\"";
        html += is_password ? "password" : "text";
        html += "\" value=\"";
        if (!is_password) html += html_escape(value);
        html += "\" autocomplete=\"off\">";
    };

    field("Wi-Fi SSID",         "wifi_ssid",     cfg.wifi_ssid,         false);
    field("Wi-Fi password",     "wifi_password", cfg.wifi_password,     true);
    field("BG API URL",         "bg_url",        cfg.bg_api_url,        false);
    field("Isla calendar URL",  "ical_isla",     cfg.ical_isla_url,     false);
    field("Personal calendar",  "ical_personal", cfg.ical_personal_url, false);
    field("Work calendar",      "ical_work",     cfg.ical_work_url,     false);

    html += F(R"HTML(<p class="note">Empty fields keep the current value. Saving reboots the device into normal mode.</p>
<div class="btn-row">
<button type="submit" class="save">Save & reboot</button>
<button type="submit" formaction="/cancel" class="cancel">Cancel</button>
</div>
</form></body></html>
)HTML");

    return html;
}

// ---- Handlers --------------------------------------------------------
static void handle_root(void)
{
    server.send(200, "text/html", render_form_html());
}

static void handle_save(void)
{
    Config c = cfg;
    auto upd = [&](String &dst, const char *name) {
        String v = server.arg(name);
        if (v.length() > 0) dst = v;
    };
    upd(c.wifi_ssid,         "wifi_ssid");
    upd(c.wifi_password,     "wifi_password");
    upd(c.bg_api_url,        "bg_url");
    upd(c.ical_isla_url,     "ical_isla");
    upd(c.ical_personal_url, "ical_personal");
    upd(c.ical_work_url,     "ical_work");

    config_save(c);
    save_then_reboot = true;
    exit_requested = true;
    server.send(200, "text/html",
                "<html><body style=\"font:16px sans-serif;padding:2em\">"
                "<h2>Saved.</h2><p>Rebooting into normal mode now…</p>"
                "</body></html>");
}

static void handle_cancel(void)
{
    save_then_reboot = false;
    exit_requested = true;
    server.send(200, "text/html",
                "<html><body style=\"font:16px sans-serif;padding:2em\">"
                "<h2>Cancelled.</h2><p>Rebooting…</p>"
                "</body></html>");
}

static void handle_not_found(void)
{
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

// ---- Entry point -----------------------------------------------------
void setup_mode_run(void)
{
    Serial.println("[setup] entering setup mode");

    // Bring up the AP. Channel 1 is fine for any region.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, 1);
    delay(150);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[setup] AP \"%s\" up, ip=%s\n",
                  AP_SSID, ip.toString().c_str());

    // Render the setup screen to the e-paper.
    build_setup_screen();
    epd_render_and_refresh();

    // Start the HTTP server.
    server.on("/",       HTTP_GET,  handle_root);
    server.on("/save",   HTTP_POST, handle_save);
    server.on("/save",   HTTP_GET,  handle_root);   // refreshing GET = re-show form
    server.on("/cancel", HTTP_POST, handle_cancel);
    server.on("/cancel", HTTP_GET,  handle_cancel);
    server.onNotFound(handle_not_found);
    server.begin();
    Serial.printf("[setup] HTTP server on port %d\n", HTTP_PORT);

    uint32_t deadline = millis() + SETUP_TIMEOUT_MS;
    while (!exit_requested && millis() < deadline) {
        server.handleClient();
        delay(5);
    }

    // Give the browser ~1s to receive the final response before we tear
    // the AP down.
    delay(1000);
    server.stop();
    WiFi.softAPdisconnect(true);

    if (save_then_reboot || exit_requested) {
        Serial.println("[setup] rebooting");
        ESP.restart();
    }

    // Idle timeout — sleep so we don't sit on the AP forever.
    Serial.println("[setup] idle timeout → deep sleep");
    epd_power_off();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)2, 0);
    esp_sleep_enable_timer_wakeup((uint64_t)600 * 1000000ULL);   // 10 min
    esp_deep_sleep_start();
}
