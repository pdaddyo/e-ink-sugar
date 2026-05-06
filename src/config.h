// Runtime configuration backed by NVS Preferences. The compile-time
// defaults in secrets.h are used only if NVS hasn't been written yet
// (first boot after flashing). The setup-mode web form is the canonical
// way to update these at runtime; saving forces ESP.restart() so the
// new values take effect.

#pragma once

#include <Arduino.h>

struct Config
{
  String wifi_ssid;
  String wifi_password;
  String bg_api_url;
  String ical_isla_url;
  String ical_personal_url;
  String ical_work_url;
};

extern Config cfg;

void config_load(void);
void config_save(const Config &c);
