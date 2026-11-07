/* Host test for stm32_charger_present_raw():
 *   gcc -Icomponents/stm32_hal/include tools/tests/charger_present_test.c -o /tmp/cp && /tmp/cp */
#include "stm32_charger_present.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* on battery: no VBUS (the BQ's VBUSV floor), not charging */
    assert(!stm32_charger_present_raw(0, 0, 0));
    assert(!stm32_charger_present_raw(0, 0, 2600));
    /* the r20 case: charger in, VBUS_STAT still 000 (no input detection) */
    assert(stm32_charger_present_raw(0, 2, 5050));   /* fast charging */
    assert(stm32_charger_present_raw(0, 3, 5050));   /* charge done, still plugged */
    assert(stm32_charger_present_raw(0, 0, 5050));   /* plugged, charging held off */
    assert(stm32_charger_present_raw(0, 1, 0));      /* pre-charge seen before VBUS ADC */
    /* a BQ that did detect the source */
    assert(stm32_charger_present_raw(1, 0, 0));
    /* the threshold itself */
    assert(stm32_charger_present_raw(0, 0, STM32_CHARGER_VBUS_MIN_MV));
    assert(!stm32_charger_present_raw(0, 0, STM32_CHARGER_VBUS_MIN_MV - 1));
    puts("charger_present_test: all passed");
    return 0;
}
