#pragma once
#include "lvgl.h"
#include "CrossPad.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadManager.hpp"

extern lv_group_t *keypad_group;
extern lv_indev_t *indev_keypad;

void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool previousPadStates[16] = {false};
    int key = -1;
    
    // Check for any pad state changes through PadManager
    for (int i = 0; i < 16; i++)
    {
        bool currentPadState = crosspad::getPadManager().isPadPressed(i);
         // Detect state change
        if (currentPadState != previousPadStates[i])
        {
            key = i;
            previousPadStates[i] = currentPadState;
            break;
        }
    }
    
    if (key == -1)
    {
        data->state = LV_INDEV_STATE_REL;
        // get the current pad pressed from GUI visualizer if none is pressed physically
        int * number = (int *)lv_indev_get_user_data(indev_keypad);
        if (number && (*number != -1))
        {
            data->key = *number;
            *number = -1; // reset the number after reading
        }
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;
        data->key = key + 1; // key numbering starts from 1
        LV_LOG_INFO("Keypad handler (key): %d, state: %d\n", data->key, data->state);
    }
}