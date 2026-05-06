#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <math.h>

#include "lvgl.h"
#include "ui/ui.h"
#include "secrets.h"

#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>

// ---- Configuration ----------------------------------------------------
// Wi-Fi credentials and personal endpoints live in secrets.h (gitignored).

#define MGDL_TO_MMOL 18.0182f
#define WIFI_TIMEOUT_MS 30000UL
#define US_PER_S 1000000ULL
#define CGM_PERIOD_S 300            // CGM publishes every 5 minutes
#define POST_DATA_DELAY 10          // wake this many seconds after each expected reading
#define SLEEP_DEFAULT_S 300         // fallback when data is missing/stale
#define SLEEP_MIN_S 60              // floor so radio always has time to come up
#define STALE_THRESHOLD_S (15 * 60) // strikethrough BG once it's older than this

// Preview / layout-test: if this GPIO is held LOW at boot (active-low button),
// or wakes the device via ext0, render synthetic worst-case data ("16.6", etc.)
// instead of fetching. Default GPIO is a guess for the Elecrow CrowPanel
// CGM-style boards — adjust to match your actual button wiring.
#define BUTTON_PIN GPIO_NUM_2

// ---- Display ----------------------------------------------------------
GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT> display(
    GxEPD2_579_GDEY0579T93(/*CS=*/45, /*DC=*/46, /*RST=*/47, /*BUSY=*/48));

#define SCR_WIDTH 792
#define SCR_HEIGHT 272
#define LVBUF ((SCR_WIDTH * SCR_HEIGHT / 8) + 8)

static lv_display_t *lvDisplay;
static uint8_t lvBuffer[2][LVBUF];

WiFiMulti wifiMulti;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

static volatile bool g_full_flush_done = false;

// ---- Latest BG snapshot (filled by fetch_bg) --------------------------
#define BG_POINTS_MAX 40

struct bg_snapshot
{
  int32_t dg_oldest_first[BG_POINTS_MAX];      // 0.1 mmol/L per point, oldest first
  long long mills_oldest_first[BG_POINTS_MAX]; // epoch ms per point, oldest first
  uint16_t point_count;
  float latest_mmol;
  float delta_mmol;
  bool have_delta;
  long long latest_mills;
  char direction[32];
  bool ok;
};

static struct bg_snapshot bg = {{0}, {0}, 0, 0.0f, 0.0f, false, 0, "", false};

// ---- Calendar agenda --------------------------------------------------
// Three calendars feed two roles:
//  - The Isla calendar's all-day events tell us who has Isla each day; that
//    becomes a per-day subtitle, not an event row.
//  - The personal/work calendars produce the event rows.
#define TZ_POSIX           "GMT0BST,M3.5.0/1,M10.5.0/2"
#define EVENTS_PER_DAY_MAX 8
// Bumped to 48 so titles like "Distributor Open Projects - Strategic" wrap
// onto a 2nd line instead of mid-word truncating; per the column-height
// math, ~6 events at 1.5-line average still fits in the 220px events box.
#define EVENT_TITLE_MAX    48

enum cal_source : uint8_t { CAL_ISLA, CAL_PERSONAL, CAL_WORK };

struct cal_event
{
  int hh;
  int mm;
  bool all_day;
  cal_source source;
  char title[EVENT_TITLE_MAX];
};

struct day_state
{
  int year;
  int month;
  int day;             // 1..31
  int wday;            // 0=Sun..6=Sat
  char date_label[16]; // e.g. "Wed 6"
  char isla_with[24];  // e.g. "PB" / "Kaz"; "" if no Isla event today
  cal_event events[EVENTS_PER_DAY_MAX];
  int event_count;
};

static struct day_state days[3]; // [today, tomorrow, day_after]

// ---- LVGL display flush ----------------------------------------------
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, unsigned char *data)
{
  int16_t width = area->x2 - area->x1 + 1;
  int16_t height = area->y2 - area->y1 + 1;
  // writeImage() only buffers — no per-tile refresh. We do a single full
  // refresh after all flushes, which clears any prior-frame ghosting.
  display.writeImage((uint8_t *)data + 8, area->x1, area->y1, width, height);
  lv_display_flush_ready(disp);

  if ((area->x1 + width == SCR_WIDTH) && (area->y1 + height == SCR_HEIGHT))
  {
    g_full_flush_done = true;
  }
}

static uint32_t my_tick(void) { return millis(); }

static void epd_setup(void)
{
  // Declare the e-paper control pins as GPIOs before display.init() does
  // its first digitalWrite() on them — silences the harmless "IO X is not
  // set as GPIO" warnings GxEPD2 otherwise emits during init.
  pinMode(45, OUTPUT); // CS
  pinMode(46, OUTPUT); // DC
  pinMode(47, OUTPUT); // RST
  pinMode(48, INPUT);  // BUSY

  SPI.begin(/*SCK=*/12, /*MISO=*/-1, /*MOSI=*/11, /*SS=*/45);
  display.init(115200, true, 2, false);
  delay(100);
}

// ---- WiFi -------------------------------------------------------------
static bool connect_wifi(void)
{
  wifiMulti.addAP(SSID_NAME, SSID_PASSWORD);
  uint32_t start = millis();
  while (wifiMulti.run() != WL_CONNECTED)
  {
    if (millis() - start > WIFI_TIMEOUT_MS)
    {
      Serial.println("[wifi] timeout, no AP");
      return false;
    }
    delay(500);
  }
  Serial.printf("[wifi] connected to %s, ip=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  return true;
}

// ---- NTP --------------------------------------------------------------
static void sync_ntp_clock(void)
{
  timeClient.begin();
  timeClient.update();
  long epoch = timeClient.getEpochTime();
  Serial.printf("[ntp] epoch=%ld\n", epoch);
  if (epoch <= 0)
    return;
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, NULL);
}

// ---- BG fetch ---------------------------------------------------------
// Nightscout entries.txt format, one entry per line, tab-separated, newest first:
//   "<dateString>" \t <mills> \t <sgv_mgdl> \t "<direction>" \t "<device>"
struct bg_entry
{
  long long mills;
  int sgv_mgdl;
  char direction[24];
};

static void strip_quotes(char *s)
{
  size_t n = strlen(s);
  if (n >= 2 && s[0] == '"' && s[n - 1] == '"')
  {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
}

// Parses one TSV line into `out`. Returns true on success.
static bool parse_bg_line(const String &line, bg_entry *out)
{
  int t1 = line.indexOf('\t');
  if (t1 < 0)
    return false;
  int t2 = line.indexOf('\t', t1 + 1);
  if (t2 < 0)
    return false;
  int t3 = line.indexOf('\t', t2 + 1);
  if (t3 < 0)
    return false;
  int t4 = line.indexOf('\t', t3 + 1);

  String s_mills = line.substring(t1 + 1, t2);
  String s_sgv = line.substring(t2 + 1, t3);
  String s_dir = line.substring(t3 + 1, t4 > 0 ? t4 : line.length());

  s_dir.trim();
  out->mills = atoll(s_mills.c_str());
  out->sgv_mgdl = atoi(s_sgv.c_str());
  strncpy(out->direction, s_dir.c_str(), sizeof(out->direction) - 1);
  out->direction[sizeof(out->direction) - 1] = '\0';
  strip_quotes(out->direction);
  return out->sgv_mgdl > 0;
}

static bool fetch_bg(void)
{
  WiFiClientSecure secure;
  secure.setInsecure();

  HTTPClient http;
  if (!http.begin(secure, BG_API_URL))
  {
    Serial.println("[bg] http.begin failed");
    return false;
  }

  int code = http.GET();
  Serial.printf("[bg] GET -> %d\n", code);
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  Stream &stream = http.getStream();
  bg_entry entries[BG_POINTS_MAX];
  uint16_t n = 0;

  uint32_t line_deadline = millis() + 5000;
  while (n < BG_POINTS_MAX)
  {
    if (millis() > line_deadline)
      break;
    String line = stream.readStringUntil('\n');
    if (line.length() == 0)
      break;
    if (parse_bg_line(line, &entries[n]))
      n++;
  }
  http.end();

  Serial.printf("[bg] parsed entries=%u\n", (unsigned)n);
  if (n == 0)
    return false;

  // Response is newest-first; reverse into oldest-first arrays.
  for (uint16_t i = 0; i < n; i++)
  {
    bg_entry &e = entries[n - 1 - i];
    bg.dg_oldest_first[i] = (int32_t)lroundf(e.sgv_mgdl * 10.0f / MGDL_TO_MMOL);
    bg.mills_oldest_first[i] = e.mills;
  }
  bg.point_count = n;
  bg.latest_mmol = entries[0].sgv_mgdl / MGDL_TO_MMOL;
  bg.latest_mills = entries[0].mills;

  strncpy(bg.direction, entries[0].direction, sizeof(bg.direction) - 1);
  bg.direction[sizeof(bg.direction) - 1] = '\0';

  bg.have_delta = false;
  if (n >= 2)
  {
    bg.delta_mmol = bg.latest_mmol - (entries[1].sgv_mgdl / MGDL_TO_MMOL);
    bg.have_delta = true;
  }

  bg.ok = true;
  Serial.printf("[bg] latest=%.1f mmol/L delta=%s%.1f dir=%s pts=%u\n",
                bg.latest_mmol,
                bg.have_delta ? (bg.delta_mmol >= 0 ? "+" : "") : "",
                bg.have_delta ? bg.delta_mmol : 0.0f,
                bg.direction,
                (unsigned)bg.point_count);
  return true;
}

// ---- iCal fetch / parse ----------------------------------------------
// Treats the proleptic Gregorian calendar as a counter (days since
// 0000-03-01). Used to compare event date ranges against today/tomorrow/
// day-after by integer comparison, sidestepping struct tm gymnastics.
static int days_since_epoch(int y, int m, int d)
{
  if (m <= 2) { y -= 1; m += 12; }
  return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1;
}

static const char *DOW_SHORT[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MONTH_SHORT[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Drops every byte with the high bit set (i.e. all multi-byte UTF-8, which
// is mostly emoji in calendar titles), then collapses runs of whitespace
// and trims the result. Lossy for non-ASCII Latin too, but the user's
// calendars are English and the goal is "no missing-glyph boxes on screen".
static void sanitize_title(char *s)
{
  if (!s) return;
  char *src = s;
  char *dst = s;
  while (*src)
  {
    if ((unsigned char)*src < 128) *dst++ = *src;
    src++;
  }
  *dst = '\0';

  // Collapse internal runs of whitespace, drop leading.
  src = s; dst = s;
  bool prev_space = true;
  while (*src)
  {
    char c = *src++;
    bool is_ws = (c == ' ' || c == '\t');
    if (is_ws)
    {
      if (!prev_space) *dst++ = ' ';
      prev_space = true;
    }
    else
    {
      *dst++ = c;
      prev_space = false;
    }
  }
  // Trim trailing space.
  if (dst > s && dst[-1] == ' ') dst--;
  *dst = '\0';
}

// Fills days[0..2] with today/tomorrow/day-after dates + a short header label.
// Requires NTP-synced wall clock; otherwise uses the device's ad-hoc time.
static void prep_days(void)
{
  for (int i = 0; i < 3; i++)
  {
    days[i] = {};
  }
  time_t now = time(NULL);
  if (now <= 0) return;
  for (int i = 0; i < 3; i++)
  {
    time_t t = now + (time_t)i * 86400;
    struct tm *lt = localtime(&t);
    days[i].year  = lt->tm_year + 1900;
    days[i].month = lt->tm_mon + 1;
    days[i].day   = lt->tm_mday;
    days[i].wday  = lt->tm_wday;
    if (i == 0)
    {
      strncpy(days[i].date_label, "Today", sizeof(days[i].date_label) - 1);
      days[i].date_label[sizeof(days[i].date_label) - 1] = '\0';
    }
    else
    {
      snprintf(days[i].date_label, sizeof(days[i].date_label),
               "%s %d", DOW_SHORT[lt->tm_wday], lt->tm_mday);
    }
  }
}

// Parses YYYYMMDD or YYYYMMDDTHHMMSS[Z] iCal date-time values.
struct ical_dt
{
  int year, month, day;
  int hour, minute;
  bool all_day;
  bool utc;
};

static bool parse_ical_dt(const String &value, ical_dt *out)
{
  if (value.length() < 8) return false;
  out->year   = value.substring(0, 4).toInt();
  out->month  = value.substring(4, 6).toInt();
  out->day    = value.substring(6, 8).toInt();
  out->hour   = 0;
  out->minute = 0;
  out->all_day = (value.length() == 8) || (value.charAt(8) != 'T');
  out->utc    = false;
  if (!out->all_day && value.length() >= 13)
  {
    out->hour   = value.substring(9, 11).toInt();
    out->minute = value.substring(11, 13).toInt();
    out->utc    = (value.charAt(value.length() - 1) == 'Z');
  }
  return out->year >= 2000;
}

// Decides if an event with start `s` and end `e` (DTEND is exclusive) covers
// the day at index `day_idx` in days[].
// For all-day events the range is [start_date, end_date) in date counts.
// For timed events: just check whether start falls on that day.
static bool event_covers_day(int day_idx, const ical_dt &s, const ical_dt &e, bool have_end)
{
  int target = days_since_epoch(days[day_idx].year, days[day_idx].month, days[day_idx].day);
  int s_day  = days_since_epoch(s.year, s.month, s.day);
  if (s.all_day)
  {
    int e_day = have_end ? days_since_epoch(e.year, e.month, e.day) : (s_day + 1);
    return s_day <= target && target < e_day;
  }
  return s_day == target;
}

// Convert an event's UTC-tagged DT to local Y/M/D/H/M by going through epoch.
static void utc_dt_to_local(ical_dt *dt)
{
  struct tm t = {};
  t.tm_year = dt->year - 1900;
  t.tm_mon  = dt->month - 1;
  t.tm_mday = dt->day;
  t.tm_hour = dt->hour;
  t.tm_min  = dt->minute;
  // timegm-equivalent: temporarily clear TZ, mktime, restore.
  setenv("TZ", "UTC0", 1); tzset();
  time_t epoch = mktime(&t);
  setenv("TZ", TZ_POSIX, 1); tzset();
  struct tm *lt = localtime(&epoch);
  dt->year   = lt->tm_year + 1900;
  dt->month  = lt->tm_mon + 1;
  dt->day    = lt->tm_mday;
  dt->hour   = lt->tm_hour;
  dt->minute = lt->tm_min;
  dt->utc    = false;
}

static void ingest_event(cal_source src, const ical_dt &start_in, const ical_dt &end_in,
                         bool have_end, const char *summary)
{
  ical_dt s = start_in;
  ical_dt e = end_in;
  if (!s.all_day && s.utc) utc_dt_to_local(&s);
  if (have_end && !e.all_day && e.utc) utc_dt_to_local(&e);

  for (int i = 0; i < 3; i++)
  {
    if (days[i].year == 0) continue;
    if (!event_covers_day(i, s, e, have_end)) continue;

    // Drop already-finished timed events from the today column.
    if (i == 0 && !s.all_day)
    {
      time_t now_s = time(NULL);
      struct tm *lt = localtime(&now_s);
      int now_min = lt->tm_hour * 60 + lt->tm_min;
      int ev_min  = s.hour * 60 + s.minute;
      if (ev_min < now_min) continue;
    }

    if (src == CAL_ISLA)
    {
      // Isla cal supplies the per-day "with whom" subtitle, not an event row.
      strncpy(days[i].isla_with, summary, sizeof(days[i].isla_with) - 1);
      days[i].isla_with[sizeof(days[i].isla_with) - 1] = '\0';
      continue;
    }

    // Defensive: also keep "PB"/"Kaz"-style all-day shorthand events out of
    // the agenda even if they slip in via the personal/work calendars.
    if (s.all_day && (strcmp(summary, "PB") == 0 || strcmp(summary, "Kaz") == 0))
    {
      continue;
    }

    // Dedup against events already added to this day. Personal and work
    // calendars frequently both carry the same meeting; first-seen wins.
    {
      bool dup = false;
      for (int j = 0; j < days[i].event_count; j++)
      {
        cal_event &existing = days[i].events[j];
        if (existing.all_day != s.all_day) continue;
        if (!s.all_day && (existing.hh != s.hour || existing.mm != s.minute)) continue;
        if (strcmp(existing.title, summary) != 0) continue;
        dup = true;
        break;
      }
      if (dup) continue;
    }

    if (days[i].event_count >= EVENTS_PER_DAY_MAX) continue;
    cal_event &ev = days[i].events[days[i].event_count++];
    ev.all_day = s.all_day;
    ev.hh      = s.hour;
    ev.mm      = s.minute;
    ev.source  = src;
    strncpy(ev.title, summary, sizeof(ev.title) - 1);
    ev.title[sizeof(ev.title) - 1] = '\0';
  }
}

// Insertion-sort each day's event list by time so morning items appear first.
// All-day events sort to the top. Called once after all calendars are fetched.
static void sort_day_events(void)
{
  for (int i = 0; i < 3; i++)
  {
    cal_event *arr = days[i].events;
    int n = days[i].event_count;
    for (int j = 1; j < n; j++)
    {
      cal_event key = arr[j];
      int key_sort = key.all_day ? -1 : key.hh * 60 + key.mm;
      int k = j - 1;
      while (k >= 0)
      {
        int k_sort = arr[k].all_day ? -1 : arr[k].hh * 60 + arr[k].mm;
        if (k_sort <= key_sort) break;
        arr[k + 1] = arr[k];
        k--;
      }
      arr[k + 1] = key;
    }
  }
}

// Reads one VEVENT block out of an iCal feed line by line.
// Stops feeding on END:VEVENT and ingests the assembled event.
struct vevent_acc
{
  bool in_event;
  bool has_start;
  bool has_end;
  ical_dt s;
  ical_dt e;
  char summary[EVENT_TITLE_MAX];
};

// Returns the part of an iCal property line after the colon, e.g.
// "DTSTART;TZID=Europe/London:20260507T140000" -> "20260507T140000".
static String ical_value(const String &line)
{
  int colon = line.indexOf(':');
  if (colon < 0) return String("");
  return line.substring(colon + 1);
}

static bool ical_starts_with(const String &line, const char *key)
{
  size_t n = strlen(key);
  if ((size_t)line.length() < n) return false;
  for (size_t i = 0; i < n; i++)
  {
    if (line.charAt(i) != key[i]) return false;
  }
  return true;
}

static bool fetch_calendar(const char *url, cal_source src, const char *tag)
{
  WiFiClientSecure secure;
  secure.setInsecure();

  HTTPClient http;
  if (!http.begin(secure, url))
  {
    Serial.printf("[cal %s] http.begin failed\n", tag);
    return false;
  }
  // Some Google endpoints redirect to an auth-protected URL when the secret
  // is wrong; let HTTPClient follow up to 3 hops.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  Serial.printf("[cal %s] GET -> %d\n", tag, code);
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  Stream &stream = http.getStream();
  vevent_acc acc = {};
  bool skip_event = false;
  uint16_t event_count = 0;
  uint16_t event_seen  = 0;

  // Date window for early skipping. Multi-day events in our 3-day forward
  // window can have started up to ~14 days ago (vacations, conferences),
  // so be generous on the past side; future side just needs to cover
  // today + tomorrow + day-after with a few days of slack.
  int today_day = (days[0].year != 0)
                  ? days_since_epoch(days[0].year, days[0].month, days[0].day)
                  : 0;
  const int PAST_WINDOW_DAYS   = 14;
  const int FUTURE_WINDOW_DAYS = 5;

  uint32_t deadline = millis() + 15000;
  while (millis() < deadline)
  {
    String line = stream.readStringUntil('\n');
    if (line.length() == 0 && !stream.available()) break;
    if (line.length() > 0 && line.charAt(line.length() - 1) == '\r')
    {
      line.remove(line.length() - 1);
    }
    if (line.length() == 0) continue;

    if (ical_starts_with(line, "BEGIN:VEVENT"))
    {
      acc = {};
      acc.in_event = true;
      skip_event = false;
      event_seen++;
      continue;
    }

    // Fast path for events we've already decided to skip: just look for
    // the closing tag, don't parse SUMMARY/DTEND/anything.
    if (skip_event)
    {
      if (ical_starts_with(line, "END:VEVENT"))
      {
        skip_event = false;
        acc.in_event = false;
      }
      continue;
    }

    if (ical_starts_with(line, "END:VEVENT"))
    {
      if (acc.in_event && acc.has_start)
      {
        ingest_event(src, acc.s, acc.e, acc.has_end, acc.summary);
        event_count++;
      }
      acc.in_event = false;
      continue;
    }
    if (!acc.in_event) continue;

    if (ical_starts_with(line, "DTSTART"))
    {
      parse_ical_dt(ical_value(line), &acc.s);
      acc.has_start = true;
      // Decide whether this event is worth fully parsing. DTSTART normally
      // arrives before DTEND/SUMMARY in Google's feeds, so we save 99% of
      // the per-event work (allocations, UTF-8 sanitisation) on the long
      // tail of out-of-window events.
      if (today_day != 0)
      {
        int s_day = days_since_epoch(acc.s.year, acc.s.month, acc.s.day);
        int diff  = s_day - today_day;
        if (diff < -PAST_WINDOW_DAYS || diff > FUTURE_WINDOW_DAYS)
        {
          skip_event = true;
        }
      }
    }
    else if (ical_starts_with(line, "DTEND"))
    {
      parse_ical_dt(ical_value(line), &acc.e);
      acc.has_end = true;
    }
    else if (ical_starts_with(line, "SUMMARY:"))
    {
      String v = ical_value(line);
      strncpy(acc.summary, v.c_str(), sizeof(acc.summary) - 1);
      acc.summary[sizeof(acc.summary) - 1] = '\0';
      sanitize_title(acc.summary);
    }
  }

  http.end();
  Serial.printf("[cal %s] %u VEVENTs seen, %u kept\n",
                tag, (unsigned)event_seen, (unsigned)event_count);
  return true;
}

static void fetch_calendars(void)
{
  fetch_calendar(ICAL_ISLA_URL,     CAL_ISLA,     "isla");
  fetch_calendar(ICAL_PERSONAL_URL, CAL_PERSONAL, "personal");
  fetch_calendar(ICAL_WORK_URL,     CAL_WORK,     "work");
  sort_day_events();

  // If today has no remaining timed events, slide the agenda forward one
  // day so the screen always shows something actionable.
  int today_timed = 0;
  for (int e = 0; e < days[0].event_count; e++)
  {
    if (!days[0].events[e].all_day) today_timed++;
  }
  if (today_timed == 0)
  {
    days[0] = days[1];
    days[1] = days[2];
    Serial.println("[cal] today has no timed events — showing tomorrow + day-after");
  }

  for (int i = 0; i < 3; i++)
  {
    Serial.printf("[cal] %s | isla=%s | %d events\n",
                  days[i].date_label,
                  days[i].isla_with[0] ? days[i].isla_with : "-",
                  days[i].event_count);
    for (int j = 0; j < days[i].event_count; j++)
    {
      cal_event &ev = days[i].events[j];
      if (ev.all_day) Serial.printf("    all-day  %s\n", ev.title);
      else            Serial.printf("    %02d:%02d    %s\n", ev.hh, ev.mm, ev.title);
    }
  }
}

// ---- Render -----------------------------------------------------------
static void format_time_since(time_t now_s, long long latest_mills, char *out, size_t cap)
{
  if (now_s <= 0 || latest_mills <= 0)
  {
    out[0] = '\0';
    return;
  }
  long secs = (long)(now_s - (time_t)(latest_mills / 1000));
  if (secs < 0)
    secs = 0;
  if (secs < 60)
  {
    snprintf(out, cap, "now");
  }
  else if (secs < 3600)
  {
    snprintf(out, cap, "%ld min", secs / 60);
  }
  else
  {
    long h = secs / 3600;
    long m = (secs % 3600) / 60;
    snprintf(out, cap, "%ldh %ldm", h, m);
  }
}

static const char *ordinal_suffix(int day)
{
  int last_two = day % 100;
  if (last_two >= 11 && last_two <= 13)
    return "th";
  switch (day % 10)
  {
  case 1:
    return "st";
  case 2:
    return "nd";
  case 3:
    return "rd";
  default:
    return "th";
  }
}

static void format_date(time_t now_s, char *out, size_t cap)
{
  if (now_s <= 0)
  {
    out[0] = '\0';
    return;
  }
  struct tm *t = localtime(&now_s);
  char month[16];
  strftime(month, sizeof(month), "%B", t);
  snprintf(out, cap, "%d%s %s", t->tm_mday, ordinal_suffix(t->tm_mday), month);
}

static void render_screen(void)
{
  String hhmm = timeClient.getFormattedTime().substring(0, 5);
  lv_label_set_text(ui_time, hhmm.c_str());

  time_t now_s = time(NULL);
  char date_buf[32];
  format_date(now_s, date_buf, sizeof(date_buf));
  lv_label_set_text(ui_date, date_buf);

  if (bg.ok)
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", bg.latest_mmol);
    lv_label_set_text(ui_bg_value, buf);
    // Single-digit readings get 4px of right-padding so they don't visually
    // butt up against the delta column.
    lv_obj_set_style_pad_right(
        ui_bg_value,
        bg.latest_mmol < 10.0f ? 4 : 0,
        LV_PART_MAIN);

    if (bg.have_delta)
    {
      snprintf(buf, sizeof(buf), "%+.1f", bg.delta_mmol);
    }
    else
    {
      buf[0] = '\0';
    }
    lv_label_set_text(ui_bg_delta, buf);

    ui_set_trend(bg.direction);

    long secs_since = (long)(now_s - (time_t)(bg.latest_mills / 1000));
    ui_set_bg_stale(secs_since > STALE_THRESHOLD_S);

    char ago[32];
    format_time_since(now_s, bg.latest_mills, ago, sizeof(ago));
    lv_label_set_text(ui_time_since, ago);

    long long now_mills = (long long)now_s * 1000LL;
    ui_chart_set_points(bg.dg_oldest_first, bg.mills_oldest_first,
                        bg.point_count, now_mills);
  }
  else
  {
    lv_label_set_text(ui_bg_value, "--.-");
    lv_label_set_text(ui_bg_delta, "");
    lv_label_set_text(ui_time_since, "no data");
    ui_set_trend(NULL);
    ui_set_bg_stale(false);
  }

  // ---- Right zone: agenda --------------------------------------------
  for (int col = 0; col < 2; col++)
  {
    ui_calendar_clear_day(col);
    if (days[col].year == 0) continue;
    ui_calendar_set_day_header(col, days[col].date_label, days[col].isla_with);
    for (int e = 0; e < days[col].event_count; e++)
    {
      const cal_event &ev = days[col].events[e];
      ui_calendar_add_event(col, ev.all_day, ev.hh, ev.mm, ev.title);
    }
  }
}

// Stuff `bg` with synthetic worst-case values so the layout's right-extent
// is visible without waiting for a real high reading.
static void populate_preview(void)
{
  bg.point_count = 0;
  bg.latest_mmol = 16.6f;
  bg.delta_mmol = -2.4f;
  bg.have_delta = true;
  time_t now_s = time(NULL);
  bg.latest_mills = (now_s > 0 ? (long long)now_s : 0LL) * 1000LL - 120000LL; // 2 min ago
  strncpy(bg.direction, "FortyFiveDown", sizeof(bg.direction) - 1);
  bg.direction[sizeof(bg.direction) - 1] = '\0';
  bg.ok = true;
}

// True if BUTTON_PIN is held LOW right now (active-low) or if the previous
// deep-sleep wake was triggered by an ext0 GPIO transition on it.
static bool button_preview_requested(void)
{
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
    return true;
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(10); // settle the pull-up
  return digitalRead(BUTTON_PIN) == LOW;
}

// Sleep until ~POST_DATA_DELAY seconds after the next expected CGM reading;
// fall back to SLEEP_DEFAULT_S whenever we don't have a recent enough fix.
static uint32_t compute_sleep_seconds(void)
{
  if (!bg.ok)
    return SLEEP_DEFAULT_S;
  time_t now_s = time(NULL);
  long secs_since = (long)(now_s - (time_t)(bg.latest_mills / 1000));
  if (secs_since < 0 || secs_since >= CGM_PERIOD_S)
    return SLEEP_DEFAULT_S;
  long target = CGM_PERIOD_S + POST_DATA_DELAY - secs_since;
  if (target < SLEEP_MIN_S)
    target = SLEEP_MIN_S;
  if (target > SLEEP_DEFAULT_S + POST_DATA_DELAY)
    target = SLEEP_DEFAULT_S + POST_DATA_DELAY;
  return (uint32_t)target;
}

// ---- Lifecycle --------------------------------------------------------
void setup(void)
{
  Serial.begin(115200);

  // Local time-zone (UK, with DST) so localtime() returns BST/GMT and the
  // agenda's "today" lines up with the user's wall-clock day.
  setenv("TZ", TZ_POSIX, 1);
  tzset();

  bool preview = button_preview_requested();
  Serial.printf("[boot] preview=%d\n", preview ? 1 : 0);

  bool wifi_ok = connect_wifi();
  if (wifi_ok)
    sync_ntp_clock();

  prep_days();

  if (preview)
  {
    populate_preview();
  }
  else if (wifi_ok)
  {
    fetch_bg();
    fetch_calendars();
  }

  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);

  epd_setup();
  lv_init();
  lv_tick_set_cb(my_tick);

  lvDisplay = lv_display_create(SCR_WIDTH, SCR_HEIGHT);
  lv_display_set_flush_cb(lvDisplay, my_disp_flush);
  lv_display_set_buffers(lvDisplay, lvBuffer[0], lvBuffer[1], LVBUF, LV_DISPLAY_RENDER_MODE_PARTIAL);

  ui_init();
  render_screen();

  while (!g_full_flush_done)
  {
    lv_timer_handler();
    delay(10);
  }

  // Single full refresh after all tiles have been buffered. Slow (~2s) but
  // wipes any prior-frame ghosting so the agenda's dense text stays clean.
  display.refresh(false);

  lv_deinit();
  digitalWrite(7, LOW);

  uint32_t sleep_s = compute_sleep_seconds();
  Serial.printf("[sleep] %u s\n", (unsigned)sleep_s);
  esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * US_PER_S);
  // Also wake on button press (active-low) so preview is on demand.
  esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

void loop(void)
{
}
