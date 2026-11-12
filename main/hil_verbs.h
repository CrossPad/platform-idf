/**
 * @file hil_verbs.h
 * @brief What the CDC verb files share: the line a verb is offered, the reply
 *        path, and every verb. hil_control.cpp holds the dispatch table; the
 *        verbs themselves are in hil_verbs_<domain>.cpp.
 */
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"

/* Defined in CrosspadPlatform.cpp beside getMixerEngine(); the metronome is the
 * sequencer's clock, so the bench needs to reach it. */
crosspad::MetronomeNode& getMetronome();
crosspad::PatternSequencer& getSequencer();
/* The sdr kit's verbs (components/crosspad-sdr), absent from every other image. */
extern "C" bool sdr_verbs_handle(const char *cmd, void (*reply)(const char *text)) __attribute__((weak));

extern std::atomic<uint32_t> g_stm_uart_ovf;

/* stm32_uart.cpp */
extern std::atomic<uint32_t> g_stm_sbuf_drop;

/* EspStm32MidiTransport.cpp */
extern std::atomic<uint32_t> g_usb_midi_tx_ok;

/* midi_esp_tinyusb_transport.cpp */
extern std::atomic<uint32_t> g_usb_midi_tx_fail;

/* One received line and the scratch the verbs share. A verb's sscanf may fill
 * a, b or name before it turns the line down, and a later verb may read what
 * is left there, so they live with the line, not in each verb. */
struct VerbLine {
    char cmd[160];
    size_t len;
    int a = -1, b = -1;
    char name[32] = {0};
};

/* A reply line to whoever sent the command: the CDC port, or the companion's
 * buffer when the verb came over BLE, SysEx or the remote session. */
void cdc_reply(const char *text);

/* hil_control.cpp: the kit a KIT_LOAD asked for, loaded on the LVGL thread. */
extern int s_pending_kit;
void async_kit_load(void *);

/* hil_control.cpp: bench pad hits, queued for the task that plays them. */
extern QueueHandle_t s_pad_inject_queue;

/* hil_control.cpp: the least main-task stack a verb has left, and that verb. */
extern uint32_t s_stack_min;
extern char s_stack_verb[20];

/* Every verb: true when it took the line. */
bool verb_pad_notes(VerbLine &l);
bool verb_usb_msc(VerbLine &l);
bool verb_net_mode(VerbLine &l);
bool verb_fw_list(VerbLine &l);
bool verb_fw_slots(VerbLine &l);
bool verb_fw_install(VerbLine &l);
bool verb_fw_switch(VerbLine &l);
bool verb_fw_confirm(VerbLine &l);
bool verb_fw_updater_verbs(VerbLine &l);
bool verb_cp_account_verbs(VerbLine &l);
bool verb_cp_remote_verbs(VerbLine &l);
bool verb_fw_ui_verbs(VerbLine &l);
bool verb_host_info_verbs(VerbLine &l);
bool verb_wifi_verbs_verbs(VerbLine &l);
bool verb_kit_convert_verbs_verbs(VerbLine &l);
bool verb_sdr_verbs(VerbLine &l);
bool verb_tbar(VerbLine &l);
bool verb_ui_state(VerbLine &l);
bool verb_twin(VerbLine &l);
bool verb_motion(VerbLine &l);
bool verb_twin_2(VerbLine &l);
bool verb_twin_3(VerbLine &l);
bool verb_pwr_gesture(VerbLine &l);
bool verb_tasks(VerbLine &l);
bool verb_toast(VerbLine &l);
bool verb_screenshot(VerbLine &l);
bool verb_in_pair(VerbLine &l);
bool verb_codec_reg(VerbLine &l);
bool verb_ingain(VerbLine &l);
bool verb_mem_blocks(VerbLine &l);
bool verb_worker(VerbLine &l);
bool verb_ulp(VerbLine &l);
bool verb_reset_log(VerbLine &l);
bool verb_imu_regw(VerbLine &l);
bool verb_imu_reg(VerbLine &l);
bool verb_stm_regw(VerbLine &l);
bool verb_stm_reg(VerbLine &l);
bool verb_reset_log_2(VerbLine &l);
bool verb_reset_log_3(VerbLine &l);
bool verb_coredump_info(VerbLine &l);
bool verb_imu_midi(VerbLine &l);
bool verb_imu_midi_2(VerbLine &l);
bool verb_heap_trace(VerbLine &l);
bool verb_touch_trace(VerbLine &l);
bool verb_touch_drag(VerbLine &l);
bool verb_touch_tap(VerbLine &l);
bool verb_topbar(VerbLine &l);
bool verb_ui_tree(VerbLine &l);
bool verb_uac_stats(VerbLine &l);
bool verb_usb_host(VerbLine &l);
bool verb_usb_enum(VerbLine &l);
bool verb_uac_late(VerbLine &l);
bool verb_uac_pkt(VerbLine &l);
bool verb_uac_flow(VerbLine &l);
bool verb_clock_status(VerbLine &l);
bool verb_imu_gesture(VerbLine &l);
bool verb_imu_ths(VerbLine &l);
bool verb_imu(VerbLine &l);
bool verb_settings_reset(VerbLine &l);
bool verb_reset_test(VerbLine &l);
bool verb_mem(VerbLine &l);
bool verb_lvgl_prof(VerbLine &l);
bool verb_lvgl_inv(VerbLine &l);
bool verb_splash(VerbLine &l);
bool verb_lvgl_stats(VerbLine &l);
bool verb_cdc_stats(VerbLine &l);
bool verb_ble_status(VerbLine &l);
bool verb_ble_devices(VerbLine &l);
bool verb_ble_stop(VerbLine &l);
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
bool verb_daw_state(VerbLine &l);
#endif
bool verb_sysex_echo(VerbLine &l);
bool verb_audio_rate(VerbLine &l);
bool verb_usb_guard(VerbLine &l);
bool verb_ota_prefs(VerbLine &l);
bool verb_ble_enable(VerbLine &l);
bool verb_ble_start(VerbLine &l);
bool verb_ble_scan(VerbLine &l);
bool verb_ble_scan_2(VerbLine &l);
bool verb_ble_connect(VerbLine &l);
bool verb_ble_disconnect(VerbLine &l);
bool verb_ble_txoff(VerbLine &l);
bool verb_ble_send(VerbLine &l);
bool verb_app_list(VerbLine &l);
bool verb_app_versions(VerbLine &l);
bool verb_app_sha(VerbLine &l);
bool verb_app_start(VerbLine &l);
bool verb_enc_focus(VerbLine &l);
bool verb_enc_group(VerbLine &l);
bool verb_enc_raw(VerbLine &l);
bool verb_enc_state(VerbLine &l);
bool verb_led_state(VerbLine &l);
bool verb_pad_stats_reset(VerbLine &l);
bool verb_midi_trs(VerbLine &l);
bool verb_midi_loop(VerbLine &l);
bool verb_stm_vpad(VerbLine &l);
bool verb_click(VerbLine &l);
bool verb_seq_dump(VerbLine &l);
bool verb_seq(VerbLine &l);
bool verb_song(VerbLine &l);
bool verb_asong_seq(VerbLine &l);
bool verb_asong_trk(VerbLine &l);
bool verb_asong_dbg(VerbLine &l);
bool verb_asong(VerbLine &l);
bool verb_outvol(VerbLine &l);
bool verb_mix_eq(VerbLine &l);
bool verb_clip(VerbLine &l);
bool verb_heap_check(VerbLine &l);
bool verb_stool(VerbLine &l);
bool verb_audio_level(VerbLine &l);
#ifdef HIL_HAS_MIXER
bool verb_mix_lvl(VerbLine &l);
bool verb_mix_preset(VerbLine &l);
bool verb_input_mon(VerbLine &l);
bool verb_mix_route(VerbLine &l);
bool verb_mix_mute(VerbLine &l);
#endif
bool verb_pad_stats(VerbLine &l);
bool verb_enc_rotate(VerbLine &l);
bool verb_enc_press(VerbLine &l);
bool verb_enc_press_2(VerbLine &l);
bool verb_app_self_close(VerbLine &l);
bool verb_app_destroy(VerbLine &l);
bool verb_app_stop(VerbLine &l);
bool verb_pad_press(VerbLine &l);
bool verb_pad_release(VerbLine &l);
bool verb_pad_pressure(VerbLine &l);
bool verb_pad_pan(VerbLine &l);
bool verb_kit_list(VerbLine &l);
bool verb_kit_status(VerbLine &l);
bool verb_settings_export(VerbLine &l);
bool verb_settings_status(VerbLine &l);
bool verb_kit_swap_log(VerbLine &l);
bool verb_wave_status(VerbLine &l);
bool verb_smpl_pace(VerbLine &l);
bool verb_smpl_stall(VerbLine &l);
bool verb_smpl_last(VerbLine &l);
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_pool(VerbLine &l);
#endif
bool verb_kit_load(VerbLine &l);
bool verb_kit_delete(VerbLine &l);
bool verb_pad_info(VerbLine &l);
bool verb_pad_mix(VerbLine &l);
bool verb_kit_save_status(VerbLine &l);
bool verb_kit_save(VerbLine &l);
bool verb_arena(VerbLine &l);
bool verb_inst_load(VerbLine &l);
bool verb_inst_release(VerbLine &l);
bool verb_inst_status(VerbLine &l);
bool verb_pitched_status(VerbLine &l);
bool verb_pitched_cap(VerbLine &l);
bool verb_simd_mode(VerbLine &l);
bool verb_sd_bench_verb_verbs(VerbLine &l);
bool verb_sd_cache(VerbLine &l);
bool verb_sd_trace(VerbLine &l);
bool verb_sd_bounce(VerbLine &l);
bool verb_pitched_bench(VerbLine &l);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_poly(VerbLine &l);
#endif
bool verb_smpl_poly_2(VerbLine &l);
bool verb_audio_buf(VerbLine &l);
bool verb_lat_trace(VerbLine &l);
bool verb_rt_prof(VerbLine &l);
bool verb_smpl_peak(VerbLine &l);
bool verb_smpl_pitch(VerbLine &l);
bool verb_smpl_loops(VerbLine &l);
bool verb_smpl_engine(VerbLine &l);
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_bkg(VerbLine &l);
#endif
bool verb_smpl_preview(VerbLine &l);
bool verb_smpl_preview_stop(VerbLine &l);
bool verb_stm_fw_info(VerbLine &l);
bool verb_stm_fw_update(VerbLine &l);
bool verb_stm_reset(VerbLine &l);
bool verb_stm_dfu(VerbLine &l);
bool verb_audio_tasks(VerbLine &l);
