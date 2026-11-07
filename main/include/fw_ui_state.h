#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * @file fw_ui_state.h
 * @brief What the Firmware views show, for FW_UI: tests drive the UI with
 *        ENC_* and read this instead of screenshots. Setters: LVGL thread;
 *        fw_ui_handle(): any task.
 *
 * FWUI: tab=<library|store|updates|wifi|-> sel=<index|-> card=<kit@version|->
 *       items=<n> selkey=<kit@version|-> host=<app|net|-> store=<state|-> busy=<0|1>
 *
 * items/sel/store describe the tab on screen. Network mode keeps every tab
 * built: a view writes them only while fw_ui_state_tab_is() its own tab, and
 * writes them again when it is shown.
 */

void fw_ui_state_set_host(const char *host);
void fw_ui_state_set_tab(const char *tab);
void fw_ui_state_set_items(uint32_t n);
void fw_ui_state_set_sel(int32_t index, const char *kit, const char *version);   /* index < 0: none */
void fw_ui_state_set_card(const char *kit, const char *version);                  /* kit nullptr: closed */
void fw_ui_state_set_store(const char *state);                                    /* fwStoreLoadName(), or nullptr */
void fw_ui_state_reset(void);
bool fw_ui_state_tab_is(const char *tab);
bool fw_ui_handle(const char *cmd, void (*reply)(const char *text));
