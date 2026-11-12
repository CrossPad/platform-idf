#ifndef GUI_H
#define GUI_H

#include "CrossPad.h"
#include "crosspad-gui/app/App.hpp"

#include "lvgl.h"

// crosspad-gui shared components (styles, widgets, components, platform)
#include "crosspad-gui/crosspad_gui.h"

// global objects
// draw user interface in app container
extern lv_obj_t * app_c;

/*********************
 *      DEFINES
 *********************/

// Initialize application registry - call once during setup
void InitializeAppRegistry();

lv_obj_t * LoadMainScreen(lv_obj_t *parent); // main screen

// miscellaneous functions
void AddApps();

/**********************
 *      MACROS
 **********************/

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define ABS(a) ((a) < 0 ? -(a) : (a))

#endif // GUI_H
