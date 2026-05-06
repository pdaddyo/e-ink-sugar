// Template for src/secrets.h — copy to src/secrets.h and fill in real values.
// secrets.h itself is gitignored.

#ifndef SUGAR_SECRETS_H
#define SUGAR_SECRETS_H

// WiFi credentials.
#define SSID_NAME         "your-wifi-ssid"
#define SSID_PASSWORD     "your-wifi-password"

// Nightscout-style BG endpoint. ?count=N controls how many recent entries
// come back; 36 covers the chart's 3-hour window at 5-minute CGM cadence.
#define BG_API_URL        "https://your-nightscout-host/api/v1/entries.txt?count=36"

// Google Calendar private iCal URLs. Get each from the calendar's "Integrate
// calendar" settings → "Secret address in iCal format". Treat as a password —
// anyone with one of these URLs can read the full calendar.
#define ICAL_ISLA_URL     "https://calendar.google.com/calendar/ical/.../basic.ics"
#define ICAL_PERSONAL_URL "https://calendar.google.com/calendar/ical/.../basic.ics"
#define ICAL_WORK_URL     "https://calendar.google.com/calendar/ical/.../basic.ics"

#endif
