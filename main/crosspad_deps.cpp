#include "crosspad_deps.h"

#include <cstdio>
#include "esp_log.h"
#include "stm32_hal_init.h"
#include "stm32_reg_map.h"

static const char* TAG = "deps";

static crosspad::VersionMatch check_lib(const char* name, crosspad::LibVersion have,
                                        uint8_t major, uint8_t minor, uint8_t patch)
{
    auto m = crosspad::compareVersion(have, major, minor, patch);
    switch (m) {
    case crosspad::VersionMatch::Exact:
        ESP_LOGI(TAG, "%s %u.%u.%u", name, have.major, have.minor, have.patch);
        break;
    case crosspad::VersionMatch::Drift:
        ESP_LOGW(TAG, "%s linked %u.%u.%u, headers %u.%u.%u — stale objects? run idf.py fullclean",
                 name, have.major, have.minor, have.patch, major, minor, patch);
        break;
    case crosspad::VersionMatch::Incompatible:
        ESP_LOGE(TAG, "%s linked %u.%u.%u, headers %u.%u.%u — INCOMPATIBLE, run idf.py fullclean",
                 name, have.major, have.minor, have.patch, major, minor, patch);
        break;
    }
    return m;
}

static bool stm_proto(uint16_t* have)
{
    stm32_link_info_t li;
    if (stm32_hal_get_link_info(&li) != ESP_OK || !li.linked) {
        *have = 0;
        return true;  // no I2C control plane (rev1) or probe pending — nothing to compare
    }
    *have = li.proto_version;
    return li.proto_version == STM32_PROTO_VERSION;
}

bool crosspad_deps_check_runtime(void)
{
    bool ok = true;
    if (check_lib("crosspad-core", crosspad::coreVersion(),
                  CROSSPAD_CORE_VERSION_MAJOR, CROSSPAD_CORE_VERSION_MINOR, CROSSPAD_CORE_VERSION_PATCH)
        == crosspad::VersionMatch::Incompatible) ok = false;
    if (check_lib("crosspad-gui", crosspad_gui::guiVersion(),
                  CROSSPAD_GUI_VERSION_MAJOR, CROSSPAD_GUI_VERSION_MINOR, CROSSPAD_GUI_VERSION_PATCH)
        == crosspad::VersionMatch::Incompatible) ok = false;

    uint16_t proto = 0;
    if (!stm_proto(&proto)) {
        ESP_LOGE(TAG, "STM32 protocol 0x%04X, this firmware speaks 0x%04X — reflash the STM32 "
                      "(CrossPad_STM32_r2) or sync crosspad/stm32/reg_map.h",
                 proto, STM32_PROTO_VERSION);
        ok = false;
    } else if (proto) {
        ESP_LOGI(TAG, "STM32 protocol 0x%04X", proto);
    }
    return ok;
}

const char* crosspad_deps_summary(char* buf, unsigned len)
{
    auto c = crosspad::coreVersion();
    auto g = crosspad_gui::guiVersion();
    bool cOk = crosspad::compareVersion(c, CROSSPAD_CORE_VERSION_MAJOR, CROSSPAD_CORE_VERSION_MINOR,
                                        CROSSPAD_CORE_VERSION_PATCH) == crosspad::VersionMatch::Exact;
    bool gOk = crosspad::compareVersion(g, CROSSPAD_GUI_VERSION_MAJOR, CROSSPAD_GUI_VERSION_MINOR,
                                        CROSSPAD_GUI_VERSION_PATCH) == crosspad::VersionMatch::Exact;
    uint16_t proto = 0;
    bool pOk = stm_proto(&proto);
    snprintf(buf, len, "core=%u.%u.%u%s gui=%u.%u.%u%s stm_proto=0x%04X/0x%04X%s",
             c.major, c.minor, c.patch, cOk ? "" : " !",
             g.major, g.minor, g.patch, gOk ? "" : " !",
             proto, (unsigned)STM32_PROTO_VERSION, pOk ? "" : " !");
    return buf;
}
