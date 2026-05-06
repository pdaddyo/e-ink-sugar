#include "config.h"
#include "secrets.h"
#include <Preferences.h>

Config cfg;

#define NS "esugar"

void config_load(void)
{
  Preferences prefs;
  prefs.begin(NS, true);
  cfg.wifi_ssid         = prefs.getString("wifi_ssid",     SSID_NAME);
  cfg.wifi_password     = prefs.getString("wifi_password", SSID_PASSWORD);
  cfg.bg_api_url        = prefs.getString("bg_url",        BG_API_URL);
  cfg.ical_isla_url     = prefs.getString("ical_isla",     ICAL_ISLA_URL);
  cfg.ical_personal_url = prefs.getString("ical_personal", ICAL_PERSONAL_URL);
  cfg.ical_work_url     = prefs.getString("ical_work",     ICAL_WORK_URL);
  prefs.end();

  Serial.printf("[cfg] wifi_ssid=%s bg_url_len=%u\n",
                cfg.wifi_ssid.c_str(),
                (unsigned)cfg.bg_api_url.length());
}

void config_save(const Config &c)
{
  Preferences prefs;
  prefs.begin(NS, false);
  prefs.putString("wifi_ssid",     c.wifi_ssid);
  prefs.putString("wifi_password", c.wifi_password);
  prefs.putString("bg_url",        c.bg_api_url);
  prefs.putString("ical_isla",     c.ical_isla_url);
  prefs.putString("ical_personal", c.ical_personal_url);
  prefs.putString("ical_work",     c.ical_work_url);
  prefs.end();
  Serial.println("[cfg] saved");
}
