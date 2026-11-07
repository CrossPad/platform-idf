#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Firmware updates: the catalog check, downloads to the SD card and installs
 * into the slot not running, each on a WiFi lease (a normal boot, or network
 * mode as the fallback). One job at a time on one task; every call here is
 * non-blocking and safe from any task. The result of the last check persists
 * in NVS "fwupd" (shared by both slots) and lights the status bar's update
 * glyph. docs/subsystems/firmware-updates.md */

typedef enum {
    FWU_IDLE, FWU_CHECKING, FWU_UP_TO_DATE, FWU_AVAILABLE, FWU_DOWNLOADING,
    FWU_INSTALLING, FWU_READY_TO_RESTART, FWU_FAILED
} fwu_state_t;

#define FWU_VERSION_LEN 24
#define FWU_KIT_LEN     16
#define FWU_ERROR_LEN   48
#define FWU_URL_LEN     128

typedef struct {
    char kit[FWU_KIT_LEN];
    char version[FWU_VERSION_LEN];
    bool beta;        /* channel */
    bool community;   /* tier */
    bool onCard;      /* a complete, verified download of it is on the card */
} FwCatalogEntry;

#ifdef __cplusplus
extern "C" {
#endif

void        fw_updater_init(void);                   /* boot, after NVS and CrosspadPlatform_Init */
fwu_state_t fw_updater_state(void);
const char *fw_updater_state_name(fwu_state_t s);     /* "idle" ... "ready_to_restart" "failed" */
bool        fw_updater_busy(void);                    /* a job is running */
bool        fw_updater_background_busy(void);         /* ...and it is a trigger's (the boot check, the daily one), not the user's */
bool        fw_updater_check_start(char *refusal, size_t n);   /* a user's check, started without waiting for the link; on false `refusal` is "busy" or "no_mem", for the caller to show -- the state and error stay as they were. The job takes the WiFi lease itself: a refusal there is a failed state with err= */
uint8_t     fw_updater_progress(void);                /* 0..100 of the transfer or install in progress */
void        fw_updater_catalog_each(bool (*fn)(const FwCatalogEntry *entry, void *ctx), void *ctx);   /* under the updater's lock; fn must not call back in; false stops */
void        fw_updater_error(char *out, size_t n);    /* "" or one token: "catalog:network", "card_full", ... */
void        fw_updater_latest(char *out, size_t n);   /* newest update of the running kit the last check found, "" none (persisted) */
int64_t     fw_updater_last_check(void);              /* server time of that check, 0 = never or no usable Date yet (persisted) */
bool        fw_updater_checked(void);                 /* a catalog check has succeeded, dated or not (persisted) */
bool        fw_updater_channel_beta(const char *kit);
bool        fw_updater_set_channel(const char *kit, bool beta);   /* persisted; false for an invalid kit */
void        fw_updater_get_url(char *out, size_t n);  /* effective base URL */
bool        fw_updater_set_url(const char *url);      /* NULL/"" = https://crosspad.app; http(s):// only; persisted */
bool        fw_updater_forget(void);                  /* factory reset: RAM and NVS "fwupd" cleared under the lock; false = erase failed */
uint32_t    fw_updater_stack_free(void);              /* the last job's unused stack, bytes */
uint32_t    fw_updater_inst_stack_free(void);         /* the last install-start task's unused internal stack, bytes; 0 before the first */
bool        fw_updater_download_start(const char *kit, const char *version, bool install, char *refusal, size_t n);   /* any catalog version of this revision ("v1.3.0" = "1.3.0"), then the install when `install`; on false `refusal` is "bad_args", "blocked", "busy" or "no_mem", for the caller to show -- the state and error stay as they were. The job takes the WiFi lease itself, first for a package this boot's last fetch saw missing from the card, otherwise only to fetch once the card has not verified it; a refused lease is a failed state with err= */
void        fw_updater_target(char *out, size_t n);          /* version the running (or last) download/install is for */
void        fw_updater_target_kit(char *out, size_t n);      /* kit of the running (or last) download/install */
void        fw_updater_ready_version(char *out, size_t n);   /* installed in the other slot this boot, waiting for a restart; "" otherwise */
void        fw_updater_launcher_ready(void);   /* normal boot, once the launcher has painted: the boot check (when enabled) and the daily idle watch */
bool        fw_updater_boot_check(void);       /* "Check at startup" */
void        fw_updater_set_boot_check(bool on);   /* persisted */
bool        fw_updater_handle(const char *cmd, void (*reply)(const char *text));   /* FW_UPD_* verbs */

#ifdef __cplusplus
}
#endif
