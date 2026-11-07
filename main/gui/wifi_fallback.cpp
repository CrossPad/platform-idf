#ifdef USE_LVGL

#include "wifi_fallback.h"

#include "crosspad-gui/components/ui_modal.h"

static net_tab_t s_tab = NET_TAB_WIFI;   // the request the open dialog belongs to

static void confirm_cb(bool yes, void * user)
{
    (void)user;
    if (yes) net_mode_request(s_tab);
}

void wifi_fallback_offer(net_tab_t tab, lv_group_t * encoder_group)
{
    if (net_mode_active()) return;
    s_tab = tab;
    ui_modal_confirm("Not enough memory",
                     "WiFi does not fit beside what is running now. Restart in network mode? "
                     "No sound and no kits until you exit.",
                     "Restart", "Cancel", confirm_cb, nullptr, encoder_group);
}

#endif // USE_LVGL
