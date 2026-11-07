/**
 * @file cp_account_verbs.cpp
 * @brief The ACCT_* verbs (docs/design/2026-10-07-account-sync-design.md).
 *
 * Run on the CDC drain loop and, for ACCT_STATUS, on the companion's main
 * task: nothing here waits for the network -- pairing runs on its own task and
 * is read back with ACCT_STATUS. No reply carries the device token. The
 * pairing code is masked in ACCT_STATUS, which also answers over BLE, where
 * whoever reads the code could claim the board; ACCT_CODE answers it in clear
 * on CDC alone (the companion allow-list names ACCT_STATUS only), to a host
 * that can already OTA any image. Neither verb logs it.
 */
#include "cp_account.h"

#include <cstdio>
#include <cstring>

#include "crosspad/fw/FwAccount.hpp"

/* The longest ACCT line: every field at its limit, the base URL at the size
 * CMake allows in practice, CRLF included; within the companion's 481-byte mailbox. */
static constexpr size_t kStatusLineMax = 320;
static constexpr char kCrlf[] = "\r\n";
static constexpr char kErrPrefix[] = "ERR ";
static constexpr char kCodeMask = '*';

/* The CDC loop and the companion poll share the main task: never concurrent.
 * The lines are built here, not on the stack: ACCT_PAIR goes on down into
 * wifi_link_acquire() and its log line from the 5 kB main task. */
static char s_line[kStatusLineMax];
static char s_refusal[CPA_ERROR_LEN];

static bool is_verb(const char* cmd, const char* verb) {
    return strcmp(cmd, verb) == 0;
}

/* noinline: its locals stay out of cp_account_handle()'s frame, which is live
 * under ACCT_PAIR's wifi_link_acquire(). */
__attribute__((noinline)) static void reply_status(void (*reply)(const char*)) {
    char code[crosspad::kFwPairCodeLen + 1];
    char err[CPA_ERROR_LEN];
    char serial[crosspad::kFwBoardSerialLen + 1];
    char label[sizeof("CP0000")];
    char name[crosspad::kFwAccountNameMax];
    cp_account_pair_code(code, sizeof code);
    for (size_t i = 0; code[i]; ++i) code[i] = kCodeMask;
    cp_account_pair_error(err, sizeof err);
    cp_account_serial(serial, sizeof serial);
    cp_account_label(label, sizeof label);
    cp_account_name(name, sizeof name);
    const bool linked = cp_account_linked();
    // name= last: a display name may hold spaces.
    snprintf(s_line, sizeof s_line,
             "ACCT: linked=%u pair=%s code=%s left=%u err=%s notice=%u stack_free=%u serial=%s label=%s base=%s "
             "name=%s\r\n",
             linked ? 1u : 0u, cp_account_pair_state_name(cp_account_pair_state()), code[0] ? code : "-",
             static_cast<unsigned>(cp_account_pair_left_s()), err[0] ? err : "-", cp_account_notice() ? 1u : 0u,
             static_cast<unsigned>(cp_account_stack_free()), serial, label[0] ? label : "-", cp_account_base(),
             linked && name[0] ? name : "-");
    reply(s_line);
}

__attribute__((noinline)) static void reply_code(void (*reply)(const char*)) {
    char code[crosspad::kFwPairCodeLen + 1];
    cp_account_pair_code(code, sizeof code);
    if (!code[0]) {
        reply("ERR no_code\r\n");
        return;
    }
    snprintf(s_line, sizeof s_line, "ACCTCODE: code=%s left=%u\r\n", code,
             static_cast<unsigned>(cp_account_pair_left_s()));
    reply(s_line);
}

__attribute__((noinline)) static void reply_pair(void (*reply)(const char*)) {
    if (cp_account_pair_start(s_refusal, sizeof s_refusal)) {
        reply("OK\r\n");
        return;
    }
    snprintf(s_line, sizeof s_line, "%s%s%s", kErrPrefix, s_refusal, kCrlf);
    reply(s_line);
}

bool cp_account_handle(const char* cmd, void (*reply)(const char*)) {
    if (is_verb(cmd, "ACCT_STATUS")) {
        reply_status(reply);
        return true;
    }
    if (is_verb(cmd, "ACCT_CODE")) {
        reply_code(reply);
        return true;
    }
    if (is_verb(cmd, "ACCT_PAIR")) {
        reply_pair(reply);
        return true;
    }
    if (is_verb(cmd, "ACCT_PAIR_CANCEL")) {
        cp_account_pair_cancel();
        reply("OK\r\n");
        return true;
    }
    if (is_verb(cmd, "ACCT_UNLINK")) {
        reply(cp_account_unlink() ? "OK\r\n" : "ERR not_linked\r\n");
        return true;
    }
    return false;
}
