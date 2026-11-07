#include "device_serial.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "crosspad/stm32/reg_map.h"
#include "esp_mac.h"
#include <cstdint>
#include <cstdio>

static constexpr uint32_t kSerialMax = 9999;   // CP0001..CP9999, as Settings shows it
static char s_pcb[sizeof("CP0000")];
static char s_mac[sizeof("000000000000")];

static bool read_pcb_serial(uint32_t* out) {
    auto* cs = crosspad::getStm32Manager().configSync();
    if (!cs) return false;
    uint8_t sn[4] = {0};
    if (!cs->readReg8(REG_IDENT_SERIAL_B3, sn[0]) || !cs->readReg8(REG_IDENT_SERIAL_B2, sn[1]) ||
        !cs->readReg8(REG_IDENT_SERIAL_B1, sn[2]) || !cs->readReg8(REG_IDENT_SERIAL_B0, sn[3])) {
        return false;
    }
    *out = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | sn[3];
    return *out <= kSerialMax;
}

const char* device_serial(void) {
    if (s_pcb[0]) return s_pcb;
    uint32_t serial = 0;
    if (read_pcb_serial(&serial)) {
        snprintf(s_pcb, sizeof s_pcb, "CP%04u", (unsigned)serial);
        return s_pcb;
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_mac, sizeof s_mac, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s_mac;
}
