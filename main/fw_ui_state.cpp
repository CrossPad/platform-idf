#include "fw_ui_state.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"

#include "fw_updater.h"
#include "fw_view_data.h"

static constexpr size_t kHostMax = 8;
static constexpr size_t kTabMax = 12;
static constexpr size_t kKeyMax = 48;   // "<kit 15>@<version 31>"
static constexpr size_t kStoreMax = 16;
static constexpr size_t kSelMax = 12;
static constexpr char kReplyFormat[] = "FWUI: tab=%s sel=%s card=%s items=%u selkey=%s host=%s store=%s busy=%u\r\n";
static constexpr size_t kConversionChars = 2 * 8;   // eight "%s"/"%u" in the format
static constexpr size_t kUnsignedDigits = 10;       // items= at its widest
static constexpr size_t kBusyDigits = 1;
/* Every field at its widest: nothing is ever cut, busy= and the CRLF least of all. */
static constexpr size_t kReplyMax = sizeof kReplyFormat - kConversionChars + (kTabMax - 1) + (kSelMax - 1) +
                                    2 * (kKeyMax - 1) + kUnsignedDigits + (kHostMax - 1) + (kStoreMax - 1) +
                                    kBusyDigits;
static constexpr char kNone[] = "-";

namespace {
struct State {
    char host[kHostMax];
    char tab[kTabMax];
    int32_t sel;
    char selkey[kKeyMax];
    char card[kKeyMax];
    uint32_t items;
    char store[kStoreMax];
};
}  // namespace

static State s_state = {"-", "-", -1, "-", "-", 0, "-"};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void put(char* dst, size_t n, const char* value) {
    taskENTER_CRITICAL(&s_mux);
    strlcpy(dst, value && value[0] ? value : kNone, n);
    taskEXIT_CRITICAL(&s_mux);
}

static void key_of(const char* kit, const char* version, char* out, size_t n) {
    if (!kit || !kit[0]) strlcpy(out, kNone, n);
    else snprintf(out, n, "%s@%s", kit, version && version[0] ? version : kNone);
}

void fw_ui_state_set_host(const char* host) { put(s_state.host, sizeof s_state.host, host); }
void fw_ui_state_set_tab(const char* tab) { put(s_state.tab, sizeof s_state.tab, tab); }
void fw_ui_state_set_store(const char* state) { put(s_state.store, sizeof s_state.store, state); }

void fw_ui_state_set_items(uint32_t n) {
    taskENTER_CRITICAL(&s_mux);
    s_state.items = n;
    taskEXIT_CRITICAL(&s_mux);
}

void fw_ui_state_set_sel(int32_t index, const char* kit, const char* version) {
    char key[kKeyMax];
    key_of(index < 0 ? nullptr : kit, version, key, sizeof key);
    taskENTER_CRITICAL(&s_mux);
    s_state.sel = index < 0 ? -1 : index;
    strlcpy(s_state.selkey, key, sizeof s_state.selkey);
    taskEXIT_CRITICAL(&s_mux);
}

void fw_ui_state_set_card(const char* kit, const char* version) {
    char key[kKeyMax];
    key_of(kit, version, key, sizeof key);
    put(s_state.card, sizeof s_state.card, key);
}

void fw_ui_state_reset(void) {
    taskENTER_CRITICAL(&s_mux);
    s_state = State{"-", "-", -1, "-", "-", 0, "-"};
    taskEXIT_CRITICAL(&s_mux);
}

bool fw_ui_state_tab_is(const char* tab) {
    taskENTER_CRITICAL(&s_mux);
    const bool same = strcmp(s_state.tab, tab) == 0;
    taskEXIT_CRITICAL(&s_mux);
    return same;
}

bool fw_ui_handle(const char* cmd, void (*reply)(const char*)) {
    if (strcmp(cmd, "FW_UI") != 0) return false;
    taskENTER_CRITICAL(&s_mux);
    const State s = s_state;
    taskEXIT_CRITICAL(&s_mux);
    char sel[kSelMax];
    if (s.sel < 0) strlcpy(sel, kNone, sizeof sel);
    else snprintf(sel, sizeof sel, "%ld", static_cast<long>(s.sel));
    /* Busy until the updater's job task has finished too: the check job holds its catalog and
     * download buffers after the views already have nothing queued. */
    const bool busy = fw_view_data_busy() || fw_updater_busy();
    char line[kReplyMax];
    snprintf(line, sizeof line, kReplyFormat, s.tab, sel, s.card, static_cast<unsigned>(s.items), s.selkey, s.host,
             s.store, busy ? 1u : 0u);
    reply(line);
    return true;
}
