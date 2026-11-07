/**
 * @file fw_updater_verbs.cpp
 * @brief The FW_UPD_* / FW_CHECK / FW_CATALOG verbs (docs/subsystems/firmware-updates.md).
 *
 * Run on the CDC drain loop and, for FW_UPD_STATUS and FW_CATALOG, on the
 * companion's main task: nothing here waits for the network or the card --
 * the work runs on the updater's job task and is read back with FW_UPD_STATUS.
 */
#include "fw_updater.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"

#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/FwKit.hpp"
#include "fw_download.h"
#include "fw_gc.h"
#include "fw_image_desc.h"

/* The companion's reply mailbox is 481 bytes; the whole FWCAT line, header
 * and CRLF included, stays within this. */
static constexpr size_t kCatalogLineMax = 448;
static constexpr size_t kCatalogHeadMax = 32;
static constexpr size_t kCatalogRowsMax = kCatalogLineMax - kCatalogHeadMax;
static constexpr size_t kCatalogRowMax = 64;
static constexpr size_t kStatusLineMax = 320;   // the longest line of every field at its limit, with its CRLF
static constexpr char kInstallWord[] = "INSTALL";
static constexpr size_t kVersionWithPrefix = FWU_VERSION_LEN + 1;   // "v1.3.0" is accepted for "1.3.0"
/* A SD card's size in MB, as 16 bits: past any card the board takes, so a typo is refused. */
static constexpr long kGcMbMax = UINT16_MAX;
/* Bounds the FW_UPD_CFG argument text: two tokens fit with room to spare, a longer line is a typo. */
static constexpr size_t kCfgArgsMax = 64;

/* The CDC loop and the companion poll share the main task: never concurrent.
 * Every reply line is built here, not on the stack: these verbs run on the
 * 5 kB main task, under hil_control_handle_cdc()'s frame, and FW_DOWNLOAD
 * goes on down into fw_slots_get(). The WiFi lease is the job's: FW_CHECK and
 * FW_DOWNLOAD answer OK before it is asked for, and a refused one is
 * state=failed err=<why> in FW_UPD_STATUS. */
static char s_line[kCatalogLineMax + 1];
static_assert(kStatusLineMax <= sizeof s_line, "the FWUPD line is built in s_line");

static constexpr char kErrPrefix[] = "ERR ";
static constexpr size_t kErrPrefixLen = sizeof kErrPrefix - 1;
static constexpr char kCrlf[] = "\r\n";
static_assert(kErrPrefixLen + FWU_ERROR_LEN + sizeof kCrlf <= sizeof s_line, "a refusal is built in s_line");

/* Where a start writes its refusal: s_line, after "ERR ". */
static char* refusal_buf() {
    memcpy(s_line, kErrPrefix, kErrPrefixLen);
    s_line[kErrPrefixLen] = '\0';
    return s_line + kErrPrefixLen;
}

/* OK, or the refusal refusal_buf() received. */
static void reply_start(bool started, void (*reply)(const char*)) {
    if (started) {
        reply("OK\r\n");
        return;
    }
    strlcat(s_line, kCrlf, sizeof s_line);
    reply(s_line);
}

/* The verb's arguments with leading spaces skipped, or nullptr when `cmd` is
 * not `verb` (FW_CHECKX is not FW_CHECK). */
static const char* args_of(const char* cmd, const char* verb) {
    const size_t n = strlen(verb);
    if (strncmp(cmd, verb, n) != 0 || (cmd[n] != '\0' && cmd[n] != ' ')) return nullptr;
    const char* a = cmd + n;
    while (*a == ' ') ++a;
    return a;
}

static const char* skip_spaces(const char* p) {
    while (*p == ' ') ++p;
    return p;
}

/* The next space-separated token into out; false when there is none or it does not fit. */
static bool take_token(const char*& p, char* out, size_t cap) {
    p = skip_spaces(p);
    const char* end = p;
    while (*end && *end != ' ') ++end;
    const size_t len = static_cast<size_t>(end - p);
    if (len == 0 || len >= cap) return false;
    memcpy(out, p, len);
    out[len] = '\0';
    p = end;
    return true;
}

/* noinline: its locals stay out of fw_updater_handle()'s frame, which is
 * live under FW_CHECK and FW_DOWNLOAD (see s_line). */
__attribute__((noinline)) static void reply_status(void (*reply)(const char*)) {
    char latest[FWU_VERSION_LEN], err[FWU_ERROR_LEN];
    fw_updater_latest(latest, sizeof latest);
    fw_updater_error(err, sizeof err);
    const char* kit = fw_image_desc_running()->kit_id;
    snprintf(s_line, sizeof s_line,
             "FWUPD: state=%s running=%s kit=%s latest=%s channel=%s progress=%u last_check=%lld err=%s "
             "stack_free=%u inst_stack_free=%u\r\n",
             fw_updater_state_name(fw_updater_state()), esp_app_get_description()->version, kit,
             latest[0] ? latest : "-", fw_updater_channel_beta(kit) ? "beta" : "stable",
             static_cast<unsigned>(fw_updater_progress()), static_cast<long long>(fw_updater_last_check()),
             err[0] ? err : "-", static_cast<unsigned>(fw_updater_stack_free()), static_cast<unsigned>(fw_updater_inst_stack_free()));
    reply(s_line);
}

struct RowSink {
    const char* kit;
    char rows[kCatalogRowsMax];
    size_t used, shown, matching;
    bool full;   // counted in total=, not shown; filter by kit to see the rest
};

static bool add_row(const FwCatalogEntry* e, void* ctx) {
    RowSink& s = *static_cast<RowSink*>(ctx);
    if (s.kit && strcmp(e->kit, s.kit) != 0) return true;
    ++s.matching;
    char row[kCatalogRowMax];
    const int w = snprintf(row, sizeof row, " | %s %s %s %s %d", e->kit, e->version, e->beta ? "beta" : "stable",
                           e->community ? "community" : "official", e->onCard ? 1 : 0);
    if (s.full || w < 0 || static_cast<size_t>(w) >= sizeof row || s.used + static_cast<size_t>(w) >= sizeof s.rows) {
        s.full = true;
        return true;
    }
    memcpy(s.rows + s.used, row, static_cast<size_t>(w) + 1);
    s.used += static_cast<size_t>(w);
    ++s.shown;
    return true;
}

static void reply_catalog(const char* kit, void (*reply)(const char*)) {
    static RowSink sink;   // main task only, see s_line
    sink = RowSink{};
    sink.kit = kit;
    fw_updater_catalog_each(add_row, &sink);
    snprintf(s_line, sizeof s_line, "FWCAT: count=%u total=%u%s\r\n", static_cast<unsigned>(sink.shown),
             static_cast<unsigned>(sink.matching), sink.rows);
    reply(s_line);
}

/* boot= persists ("Check at startup"); gc_mb= is a test knob for the library limit,
 * RAM only. Every token is checked before any is applied: a typo changes
 * nothing. noinline: see reply_status(). */
__attribute__((noinline)) static bool apply_cfg(const char* args) {
    char buf[kCfgArgsMax];
    if (strlcpy(buf, args, sizeof buf) >= sizeof buf) return false;
    long gcMb = -1;
    int boot = -1;
    int tokens = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(nullptr, " ", &save)) {
        char* eq = strchr(tok, '=');
        if (!eq) return false;
        *eq = '\0';
        const char* value = eq + 1;
        char* end = nullptr;
        const long v = strtol(value, &end, 10);
        if (!*value || *end) return false;
        if (strcmp(tok, "boot") == 0 && (v == 0 || v == 1)) boot = static_cast<int>(v);
        else if (strcmp(tok, "gc_mb") == 0 && v >= 0 && v <= kGcMbMax) gcMb = v;
        else return false;
        ++tokens;
    }
    if (tokens == 0) return false;
    if (boot >= 0) fw_updater_set_boot_check(boot == 1);
    if (gcMb >= 0) fw_gc_set_library_limit_mb(static_cast<uint32_t>(gcMb));
    return true;
}

bool fw_updater_handle(const char* cmd, void (*reply)(const char*)) {
    const char* a;
    if ((a = args_of(cmd, "FW_UPD_STATUS"))) {
        reply_status(reply);
        return true;
    }
    if ((a = args_of(cmd, "FW_CHECK"))) {
        reply_start(fw_updater_check_start(refusal_buf(), FWU_ERROR_LEN), reply);
        return true;
    }
    if ((a = args_of(cmd, "FW_DOWNLOAD"))) {
        /* Exactly "<kit> <version> [INSTALL]": a token that does not fit its buffer is
         * refused, never cut (a cut version is a different version). */
        char kit[FWU_KIT_LEN], version[kVersionWithPrefix], word[sizeof kInstallWord];
        const char* p = a;
        const bool shape = take_token(p, kit, sizeof kit) && take_token(p, version, sizeof version);
        const bool install = shape && *skip_spaces(p) != '\0';
        if (!shape || (install && (!take_token(p, word, sizeof word) || strcmp(word, kInstallWord) != 0)) ||
            *skip_spaces(p) != '\0') {
            reply("ERR FW_DOWNLOAD <kit> <version> [INSTALL]\r\n");
        } else {
            reply_start(fw_updater_download_start(kit, version, install, refusal_buf(), FWU_ERROR_LEN), reply);
        }
        return true;
    }
    if ((a = args_of(cmd, "FW_UPD_CFG"))) {
        reply(apply_cfg(a) ? "OK\r\n" : "ERR FW_UPD_CFG [boot=0|1] [gc_mb=<n>]\r\n");
        return true;
    }
    if ((a = args_of(cmd, "FW_CATALOG"))) {
        reply_catalog(*a ? a : nullptr, reply);
        return true;
    }
    if ((a = args_of(cmd, "FW_UPD_URL"))) {
        if (!*a) {
            reply("ERR FW_UPD_URL <url>|DEFAULT\r\n");
            return true;
        }
        reply(fw_updater_set_url(strcmp(a, "DEFAULT") == 0 ? nullptr : a) ? "OK\r\n" : "ERR bad_url\r\n");
        return true;
    }
    if ((a = args_of(cmd, "FW_UPD_CHANNEL"))) {
        /* Exactly "<kit> stable|beta": a token cut to fit a buffer is a different kit. */
        const char* space = strchr(a, ' ');
        const size_t kitLen = space ? static_cast<size_t>(space - a) : 0;
        char kit[FWU_KIT_LEN];
        const char* channel = space ? space + 1 : "";
        const bool isBeta = strcmp(channel, "beta") == 0;
        if (kitLen == 0 || kitLen >= sizeof kit || (!isBeta && strcmp(channel, "stable") != 0)) {
            reply("ERR FW_UPD_CHANNEL <kit> stable|beta\r\n");
            return true;
        }
        memcpy(kit, a, kitLen);
        kit[kitLen] = '\0';
        reply(crosspad::fwKitIdValid(kit) && fw_updater_set_channel(kit, isBeta) ? "OK\r\n" : "ERR bad_kit\r\n");
        return true;
    }
    return false;
}
