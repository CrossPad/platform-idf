// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file fw_view_service.cpp
 * @brief crosspad-gui's Firmware views on this board: IFwViewService over the
 *        card worker (fw_view_data), the slots and their flows, the updater,
 *        the WiFi lease, network mode and FW_UI. Every call is a forward; the
 *        threading rules are those of the functions forwarded to.
 */
#include "fw_view_service.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "crosspad-gui/fw/IFwViewService.h"
#include "fw_flow_overlay.h"
#include "fw_gc.h"
#include "fw_slots.h"
#include "fw_ui_state.h"
#include "fw_updater.h"
#include "fw_view_data.h"
#include "net_mode.h"
#include "wifi_fallback.h"
#include "wifi_link.h"

using crosspad_gui::FwNetTab;
using crosspad_gui::FwUpdaterState;

static_assert(crosspad_gui::kFwuVersionMax == FWU_VERSION_LEN && crosspad_gui::kFwuKitMax == FWU_KIT_LEN &&
                  crosspad_gui::kFwuErrorMax == FWU_ERROR_LEN,
              "the views' buffers are the updater's");
static_assert(static_cast<int>(FwUpdaterState::Failed) == FWU_FAILED &&
                  static_cast<int>(FwUpdaterState::ReadyToRestart) == FWU_READY_TO_RESTART,
              "the views number the updater's states as it does");
static_assert(static_cast<int>(crosspad_gui::kFwNetTabStore) == NET_TAB_STORE &&
                  static_cast<int>(crosspad_gui::kFwNetTabUpdates) == NET_TAB_UPDATES &&
                  static_cast<int>(crosspad_gui::kFwNetTabCount) == NET_TAB_COUNT,
              "the views name network mode's tabs as it does");
static_assert(FW_KIT_ID_LEN == crosspad::kFwKitMax, "kit ids are core's");

namespace {

class IdfFwViewService final : public crosspad_gui::IFwViewService {
public:
    void dataOpen() override { fw_view_data_open(); }
    void dataClose() override { fw_view_data_close(); }
    bool dataBusy() override { return fw_view_data_busy(); }
    void dataPoll() override { fw_view_data_poll(); }
    void scanLibrary() override { fw_view_data_scan_library(); }
    crosspad_gui::FwLibrarySnap* library() override { return fw_view_data_library(); }
    uint32_t libraryGen() override { return fw_view_data_library_gen(); }
    void loadStore() override { fw_view_data_load_store(); }
    void dropStore() override { fw_view_data_drop_store(); }
    const crosspad::FwStore* store() override { return fw_view_data_store(); }
    crosspad::FwStoreLoad storeState() override { return fw_view_data_store_state(); }
    bool storeLoaded() override { return fw_view_data_store_loaded(); }
    uint32_t storeGen() override { return fw_view_data_store_gen(); }
    void loadDetail(const char* kit, const char* version, const char* pkg) override {
        fw_view_data_load_detail(kit, version, pkg);
    }
    const crosspad::FwKitDetail* detail() override { return fw_view_data_detail(); }
    uint32_t detailGen() override { return fw_view_data_detail_gen(); }
    void setPinned(const char* pkg, bool pinned) override { fw_view_data_set_pinned(pkg, pinned); }
    void remove(const char* pkg, const char* slotA, const char* slotB) override {
        fw_view_data_remove(pkg, slotA, slotB);
    }
    void ackCommunity(const char* kit) override { fw_view_data_ack_community(kit); }
    bool communityOk(const char* kit) override { return fw_view_data_community_ok(kit); }
    bool requestIcon(uint32_t ticket, const char* pkg, const char* kit, const char* version) override {
        return fw_view_data_request_icon(ticket, pkg, kit, version);
    }
    void cancelIcon(uint32_t ticket) override { fw_view_data_cancel_icon(ticket); }
    void freeIcon(uint8_t* buf) override { heap_caps_free(buf); }
    uint32_t libraryLimitMb() override { return fw_gc_library_limit_mb(); }

    void readSlots(crosspad::FwSlotInfo out[2]) override { fw_slots_get(out); }
    bool runningOnTrial() override { return fw_running_on_trial(); }
    bool rollbackSupported() override { return fw_rollback_supported(); }
    bool installWriting() override {
        const FwInstallPhase phase = fw_install_status().phase;
        return phase == FwInstallPhase::Verifying || phase == FwInstallPhase::Writing;
    }
    bool flowActive() override { return fw_flow_active(); }
    bool flowInstallAndRestart(const char* pkg, bool allowUnknownBuild) override {
        return fw_flow_install_and_restart(pkg, allowUnknownBuild);
    }
    bool flowSwitchAndRestart(bool clearRollback) override { return fw_flow_switch_and_restart(clearRollback); }
    const char* runningVersion() override { return esp_app_get_description()->version; }

    FwUpdaterState updaterState() override { return static_cast<FwUpdaterState>(fw_updater_state()); }
    bool updaterBusy() override { return fw_updater_busy(); }
    uint8_t updaterProgress() override { return fw_updater_progress(); }
    void updaterError(char* out, size_t n) override { fw_updater_error(out, n); }
    void updaterTarget(char* out, size_t n) override { fw_updater_target(out, n); }
    void updaterTargetKit(char* out, size_t n) override { fw_updater_target_kit(out, n); }
    void updaterReadyVersion(char* out, size_t n) override { fw_updater_ready_version(out, n); }
    bool updaterChecked() override { return fw_updater_checked(); }
    int64_t updaterLastCheck() override { return fw_updater_last_check(); }
    bool channelBeta(const char* kit) override { return fw_updater_channel_beta(kit); }
    bool checkStart(char* refusal, size_t n) override { return fw_updater_check_start(refusal, n); }
    bool downloadStart(const char* kit, const char* version, bool install, char* refusal, size_t n) override {
        return fw_updater_download_start(kit, version, install, refusal, n);
    }

    bool wifiAcquire(const char* reason) override { return wifi_link_acquire(reason); }
    void wifiRelease() override { wifi_link_release(); }
    crosspad::WifiLinkState wifiState() override { return static_cast<crosspad::WifiLinkState>(wifi_link_state()); }
    crosspad::WifiFail wifiFail() override { return static_cast<crosspad::WifiFail>(wifi_link_fail()); }
    bool networkMode() override { return net_mode_active(); }
    void offerNetworkMode(FwNetTab tab, lv_group_t* group) override {
        wifi_fallback_offer(static_cast<net_tab_t>(tab), group);
    }
    int64_t nowUs() override { return esp_timer_get_time(); }

    void uiHost(const char* host) override { fw_ui_state_set_host(host); }
    void uiTab(const char* tab) override { fw_ui_state_set_tab(tab); }
    bool uiTabIs(const char* tab) override { return fw_ui_state_tab_is(tab); }
    void uiItems(uint32_t n) override { fw_ui_state_set_items(n); }
    void uiSel(int32_t index, const char* kit, const char* version) override {
        fw_ui_state_set_sel(index, kit, version);
    }
    void uiCard(const char* kit, const char* version) override { fw_ui_state_set_card(kit, version); }
    void uiStore(const char* state) override { fw_ui_state_set_store(state); }
    void uiReset() override { fw_ui_state_reset(); }
};

IdfFwViewService s_service;

}  // namespace

void fw_view_service_install(void) { crosspad_gui::setFwViewService(&s_service); }
