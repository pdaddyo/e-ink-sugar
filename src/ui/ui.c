// Hand-written UI entry (formerly SquareLine-generated).
// Per-screen globals are defined in their respective screen files.

#include "ui.h"
#include "ui_helpers.h"

void ui_init(void)
{
    ui_Screen1_screen_init();
    lv_screen_load(ui_Screen1);
}
