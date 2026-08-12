#ifndef WMS_GUI_H
#define WMS_GUI_H

#include "core.h"

/* Runs the raylib window + main loop. Returns when the user closes it. */
void gui_run(WmsDb *db, int current_user_id);

#endif
