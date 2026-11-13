#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The board's link to a crosspad.app account (docs/design/2026-10-07-account-sync-design.md).
 * Identity: the ESP's factory MAC. Pairing: a code shown on the LCD, entered on
 * the website, then a device token kept in NVS "cpacct" (shared by both slots,
 * erased by a factory reset). The token is attached by fw_http alone, only to
 * URLs under the compile-time CROSSPAD_ACCOUNT_URL, never logged and never in
 * a verb's reply. NVS is written on a one-shot internal-stack task; pairing's
 * HTTP runs on a PSRAM-stack task holding a WiFi lease. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CPA_IDLE, CPA_STARTING, CPA_CODE, CPA_LINKED, CPA_EXPIRED, CPA_FAILED } cpa_pair_t;
#define CPA_ERROR_LEN 24

void        cp_account_init(void);                        /* boot, main task, after NVS: token and name into RAM, the MAC */
bool        cp_account_linked(void);
void        cp_account_name(char *out, size_t n);         /* "" when not linked */
void        cp_account_serial(char *out, size_t n);       /* 12 lower-case hex: the pairing identity */
void        cp_account_label(char *out, size_t n);        /* "CP1234" when the STM reported a PCB serial at the last ACCT_PAIR, else "" */
const char *cp_account_base(void);                        /* CROSSPAD_ACCOUNT_URL */
bool        cp_account_auth_header(const char *url, char *out, size_t n, uint32_t *gen);   /* "Bearer <token>": linked and url under the base; *gen: the link it belongs to; any task */
void        cp_account_revoked(uint32_t gen);             /* a 401 to a request that carried link `gen`'s token; ignored once the board linked again; any task */
bool        cp_account_notice(void);                      /* "unlinked - link again" is to be shown; cleared by a link or an unlink */
bool        cp_account_pair_start(char *refusal, size_t n);   /* lease on the caller's task; false: "busy" or the lease refusal; true while a cancelled pairing still ends: the new one follows it */
void        cp_account_pair_cancel(void);
cpa_pair_t  cp_account_pair_state(void);
const char *cp_account_pair_state_name(cpa_pair_t s);    /* "idle" "starting" "code" "linked" "expired" "failed" */
void        cp_account_pair_code(char *out, size_t n);    /* "" unless CPA_CODE */
uint32_t    cp_account_pair_left_s(void);
void        cp_account_pair_error(char *out, size_t n);   /* "" or a token: "no_network", "http", "parse", "network" */
bool        cp_account_unlink(void);                      /* false: not linked */
bool        cp_account_forget(void);                      /* factory reset: RAM now, NVS on the one-shot task */
uint32_t    cp_account_stack_free(void);                  /* the last pairing task's unused stack, bytes */
bool        cp_account_handle(const char *cmd, void (*reply)(const char *text));   /* ACCT_* */

#ifdef __cplusplus
}
#endif
