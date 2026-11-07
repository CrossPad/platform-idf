#pragma once

#include "crosspad-gui/app/App.hpp"

lv_obj_t * lv_CreateUpdateApp(lv_obj_t * parent, App * a);
void lv_UpdateApp_destroy(lv_obj_t * app);
