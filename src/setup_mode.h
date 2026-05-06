// Setup mode: device runs a Wi-Fi AP and a small HTTP form so the user
// can edit the runtime config without re-flashing. Triggered by the same
// button that previously fired the layout-preview mode. Never returns —
// either ESP.restart()s after a save, or deep-sleeps after the timeout.

#pragma once

void setup_mode_run(void) __attribute__((noreturn));
