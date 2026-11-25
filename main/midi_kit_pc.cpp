/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @file midi_kit_pc.cpp
 * @brief A host's Program Change (with Bank Select) changes the board's kit.
 *
 * The whole design is one rule: a MIDI callback may not do anything a MIDI
 * callback must not do. The parsers run on the uartLoop task
 * (main/midiCallbacks.cpp:684-747), whose job it is to drain MIDI and to play
 * injected hits, and which the docs already warn must not be blocked
 * (docs/subsystems/kit-swap.md, main/hil_control.cpp:459-464 -- a pad hit that
 * waits 1.1 s on the card made every CDC reply time out). So a Program Change
 * here is parked in one atomic word and applied on the LVGL thread, where the
 * kit model lives and where the loader is allowed to be called from
 * (crosspad-sampler/include/crosspad-sampler/logic/kit_load_task.hpp:18-31).
 *
 * Park-and-coalesce, newest wins, is the pattern main/hil_control.cpp:397-420
 * already uses for a host riding pad levels: a host sending five program changes
 * in a row -- a DAW restoring a bank, someone scrolling a patch list -- ends in
 * one load of the last one, and at most one node in the LVGL async queue.
 *
 * Why the console line is logged with the tag "hil_control": crosspad-hil's
 * kit_churn waits on the literal marker `hil_control: KIT_LOAD <n>
 * queued|started` (crosspad-hil/crosspad_hil/knowledge/markers.yaml:75), and a
 * kit change asked for over MIDI should be as visible to the harness as one
 * asked for over CDC. The line stays byte-identical on purpose; the module's own
 * line names where the request came from.
 */

#include "include/midi_kit_pc.h"
#include "include/kit_pc.h"
#include "include/hil_control.h"        /* g_kit_scan_done */
#include "crosspad/midi/MidiInputHandler.hpp"   /* the fan-out the pads' notes and the encoder's CC 1 use */
#include "include/lv_async_safe.h"

#include "NvsSettingsStore.hpp"
#include "crosspad/control/KitList.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"

#include "esp_log.h"

#include <atomic>
#include <cstdio>
#include <cstring>

static const char *TAG = "kit_pc";

/* The setting. Its own namespace, like the USB host placement (main/
 * usb_host_profile.cpp:11-12), so a settings export/import and a
 * SETTINGS_RESET meet it as a pair of keys rather than as fields of a struct
 * that crosspad-core owns and its PRs are reorganising. */
static const char *kNs = "cfg_midi";
static const char *kKeyMode = "pc_kit";
static const char *kKeyCh = "pc_ch";

/* Shipping default. A product decision, not a technical one: with KIT_PC_FLAT a
 * board answers a Program Change from the first boot, with KIT_PC_OFF it keeps
 * exactly the behaviour the manual describes ("CrossPad odbiera nuty i zegar",
 * docs/manual/chapters/17-midi.html:7). The bench wants the feature visible, and
 * the manual row this change adds says what it does either way. */
static constexpr kit_pc_mode_t kDefaultMode = KIT_PC_FLAT;

static std::atomic<int> s_mode{static_cast<int>(kDefaultMode)};
/* Two views of the same filter: `s_channel` is what the core API wants (0-based,
 * -1 = any), `s_publicCh` is what a person and a DAW say (0 = any, 1..16).
 * Keeping both, in the units each side speaks, is what stops the off-by-one the
 * existing handleSample* forwards carry (main/midiCallbacks.cpp:633-680). */
static std::atomic<int> s_channel{-1};
static std::atomic<int> s_publicCh{0};

/* One parked request: valid, channel (the library's 1..16), bank MSB, bank LSB,
 * program. Written only by the MIDI task; read-and-cleared on the LVGL thread. */
static std::atomic<uint32_t> s_parked{0};
static std::atomic<bool> s_posted{false};

/* Bank Select, latched per channel until the next Program Change. CC 0 is MSB,
 * CC 32 LSB (GM/GM2). Only the MIDI task touches these two arrays. */
static uint8_t s_msb[16];
static uint8_t s_lsb[16];

static std::atomic<uint32_t> s_seen{0}, s_loaded{0}, s_refused{0}, s_ignored{0};

/* Announcing the kit the board is on (see midi_kit_pc.h for the contract). On by
 * default, not persisted: like the Control Change map, this is a property of the
 * session a host is in rather than something a board should remember across a
 * reboot. */
static std::atomic<int> s_echo{1};
static std::atomic<int> s_announced{-1};
static std::atomic<bool> s_announce_posted{false};
static std::atomic<uint32_t> s_announceN{0};
static std::atomic<int> s_lastKit{-1};
static std::atomic<int> s_lastWhy{static_cast<int>(crosspad::kitpc::Why::Off)};

constexpr uint32_t kValidBit = 1u << 31;
constexpr int kChannel = 0, kLoaded = 1, kRefused = 2, kIgnored = 3;

crosspad::kitpc::Mode mode_now()
{
    const int m = s_mode.load();
    return m == static_cast<int>(KIT_PC_FLAT) ? crosspad::kitpc::Mode::Flat
                                              : m == static_cast<int>(KIT_PC_GROUP)
                                                    ? crosspad::kitpc::Mode::Group
                                                    : crosspad::kitpc::Mode::Off;
}

/* The board's kit listing, read where it is allowed to be read. Nothing here
 * keeps a Slot beyond one line: a kit model is not a snapshot, and holding a
 * name across a call would hand a freed string to the reply. */
struct ManagerListing {
    crosspad::IKitManager *mgr;

    size_t count() const { return static_cast<size_t>(mgr->getKitCount()); }

    crosspad::kitpc::Slot at(size_t i) const
    {
        crosspad::kitpc::Slot s;
        crosspad::KitInfo *kit = mgr->getKit(static_cast<int>(i));
        if (!kit) return s;                       /* a hole in the listing */
        s.id = static_cast<int>(i);
        s.loadable = !kit->path.empty();          /* a deleted kit keeps its slot */
        s.name = kit->name.c_str();
        /* The kit browser's own buckets (docs/subsystems/sample-tools.md,
         * "Kit Types"): the engine first, then what the folder says. */
        if (kit->engine == crosspad::KitEngine::Pitched)      s.group = crosspad::kitpc::Group::Pitch;
        else if (kit->engine == crosspad::KitEngine::Slice)   s.group = crosspad::kitpc::Group::Slice;
        else if (kit->isDrums())                              s.group = crosspad::kitpc::Group::Drums;
        else if (kit->isUser())                               s.group = crosspad::kitpc::Group::User;
        else                                                  s.group = crosspad::kitpc::Group::Perf;
        return s;
    }
};

/* On the LVGL thread: the lookup, the guard, the hand-over to the loader. */
static void apply_parked(void *)
{
    s_posted.store(false);
    const uint32_t p = s_parked.exchange(0);
    if (!(p & kValidBit)) return;

    const int libCh = static_cast<int>((p >> 24) & 0x1F);          /* 1..16 */
    const uint8_t msb = static_cast<uint8_t>((p >> 16) & 0xFF);
    const uint8_t lsb = static_cast<uint8_t>((p >> 8) & 0xFF);
    const uint8_t program = static_cast<uint8_t>(p & 0x7F);
    const int ch0 = libCh > 0 ? libCh - 1 : 0;                     /* core is 0-based */

    crosspad::IKitManager *mgr = crosspad::getKitManager();
    if (!mgr) {
        s_lastWhy.store(static_cast<int>(crosspad::kitpc::Why::Empty));
        ++s_refused;
        return;
    }
    const crosspad::kitpc::Decision d =
        crosspad::kitpc::resolve(ManagerListing{mgr}, mode_now(), s_channel.load(), ch0,
                                 msb, lsb, program);
    s_lastKit.store(d.kitId);
    s_lastWhy.store(static_cast<int>(d.why));

    if (d.why != crosspad::kitpc::Why::Ok) {
        /* A refusal gets a line, because the alternative is a host that sent a
         * message and heard nothing at all -- the exact complaint every
         * "MIDI doesn't work" thread in the forum turns out to be. The name
         * of the reason is what makes it fixable from the host's side. */
        ESP_LOGW(TAG, "PC ch%u pc%u msb%u lsb%u -> %s", (unsigned)libCh, (unsigned)program,
                 (unsigned)msb, (unsigned)lsb, crosspad::kitpc::whyName(d.why));
        (d.why == crosspad::kitpc::Why::Channel || d.why == crosspad::kitpc::Why::Off ? s_ignored
                                                                                       : s_refused)
            .fetch_add(1);
        return;
    }
    /* The same two guards the CDC verb and the SysEx sub use, in the same order
     * (main/hil_control.cpp:449-457, :4691-4705): a listing mid-scan is a moved
     * buffer, and an id the manager does not hold is a bug elsewhere. */
    if (!g_kit_scan_done.load()) {
        ESP_LOGW(TAG, "PC -> kit %d refused: the card is still being scanned", d.kitId);
        ++s_refused;
        return;
    }
    if (!crosspad::control::kitIdLoadable(d.kitId)) {
        ESP_LOGW(TAG, "PC -> kit %d refused: the manager does not hold it", d.kitId);
        ++s_refused;
        return;
    }

    const int cur = mgr->getCurrentKitId();
    if (cur == d.kitId) {
        /* Not an error, and not a load: asking the loader for the kit it already
         * has costs a card pass for nothing. A host that sends the same program
         * again gets a quiet line and an unchanged kit. */
        ESP_LOGD(TAG, "PC -> kit %d already current", d.kitId);
        ++s_loaded;
        return;
    }

    /* Seeded before the load: this kit is on its way because a host asked for it,
     * so announcing it back at that host would be an echo, not news. The load is
     * asynchronous, so without this the announcement job would see the old kit
     * still current, then the new one, and would tell the host twice. A TRS cable
     * looped from the board's MIDI OUT into its MIDI IN comes back here as well,
     * where `cur == d.kitId` above breaks that loop by design. */
    s_announced.store(d.kitId);

    const bool busy = crosspad_sampler::kit_load_task_is_loading();
    crosspad_sampler::kit_load_task_start(false, d.kitId);
    ESP_LOGI(TAG, "PC ch%u pc%u msb%u lsb%u -> kit %d (%s idx %d)", (unsigned)libCh,
             (unsigned)program, (unsigned)msb, (unsigned)lsb, d.kitId,
             crosspad::kitpc::modeName(mode_now()), d.index);
    ESP_LOGI("hil_control", "KIT_LOAD %d %s", d.kitId, busy ? "queued" : "started");
    ++s_loaded;
}

/* The channel to announce on: the one the feature is filtered to, or channel 1
 * when it listens on any -- a host that set `KIT_PC CH 5` gets the answer where it
 * asked the questions. */
static int announce_channel0(void)
{
    const int f = s_channel.load();
    if (f >= 0) return f;
    const int pub = s_publicCh.load();
    return (pub >= 1 && pub <= 16) ? pub - 1 : 0;
}

/* The announcement itself, on the LVGL thread, where the kit listing may be read
 * (the same reason apply_parked runs here). Sends what a host would have sent:
 * Bank Select MSB, then the program, in the mode the host is being shown by
 * `KIT_PC MAP` -- so a setlist written against the map and the board's own
 * announcements are the same language. The LSB is left out because neither mode
 * uses it in its addressing. */
static void announce_current(void *)
{
    s_announce_posted.store(false);
    if (s_echo.load() == 0) return;
    const kit_pc_mode_t m = static_cast<kit_pc_mode_t>(s_mode.load());
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    if (!mgr || m == KIT_PC_OFF || !g_kit_scan_done.load()) return;
    const int cur = mgr->getCurrentKitId();
    if (cur < 0 || cur == s_announced.load()) return;
    const ManagerListing listing{mgr};
    const int pos = crosspad::kitpc::positionOf(listing, mode_now(), cur);
    uint8_t msb = 0, pc = 0;
    if (pos < 0 || !crosspad::kitpc::pairAt(listing, mode_now(), static_cast<size_t>(pos), &msb, &pc)) {
        /* A kit the listing does not place -- deleted under the loader, or a card
         * that changed since the scan -- has no addressing to announce. Remember
         * the kit anyway, so this is not retried ten times a second, and say so
         * where a host looking for an answer can find the line. */
        s_announced.store(cur);
        ESP_LOGD(TAG, "kit %d has no program addressing in %s mode: nothing announced",
                 cur, crosspad::kitpc::modeName(mode_now()));
        return;
    }
    const int ch0 = announce_channel0();
    auto &sink = crosspad::getMidiInputHandler();
    sink.sendControlChange(0, msb, static_cast<uint8_t>(ch0));
    sink.sendProgramChange(pc, static_cast<uint8_t>(ch0));
    s_announced.store(cur);
    ++s_announceN;
    ESP_LOGI(TAG, "kit %d announced: ch%u msb%u pc%u (%s)", cur, (unsigned)(ch0 + 1),
             (unsigned)msb, (unsigned)pc, crosspad::kitpc::modeName(mode_now()));
}

/* The cheap half, from a periodic job: read one int, compare, post at most one
 * announcement. Nothing here walks the kit listing, which is the LVGL thread's. */
extern "C" int kit_pc_check_announce(void)
{
    if (s_echo.load() == 0) return 0;
    if (static_cast<kit_pc_mode_t>(s_mode.load()) == KIT_PC_OFF) return 0;
    if (!g_kit_scan_done.load()) return 0;
    if (crosspad_sampler::kit_load_task_is_loading()) return 0;
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    if (!mgr) return 0;
    const int cur = mgr->getCurrentKitId();
    if (cur < 0 || cur == s_announced.load()) return 0;
    if (s_announce_posted.exchange(true)) return 0;
    lv_async_call_locked(announce_current, nullptr);
    return 1;
}

extern "C" void kit_pc_set_echo(int on)
{
    s_echo.store(on ? 1 : 0);
    if (!on) return;
    /* Turning it back on starts from where the board is now: a switch is not a
     * reason to tell a host about every kit that changed while it was off. */
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    s_announced.store(mgr ? mgr->getCurrentKitId() : -1);
}

extern "C" int kit_pc_echo(void) { return s_echo.load(); }
extern "C" int kit_pc_announced(void) { return s_announced.load(); }
extern "C" uint32_t kit_pc_announce_count(void) { return s_announceN.load(); }

extern "C" void kit_change_control_change(uint8_t ch, uint8_t cc, uint8_t val)
{
    if (ch < 1 || ch > 16) return;
    const size_t i = ch - 1;
    if (cc == 0) {
        s_msb[i] = val & 0x7F;
        if (mode_now() != crosspad::kitpc::Mode::Off) {
            ESP_LOGD(TAG, "bank ch%u msb=%u", (unsigned)ch, (unsigned)(val & 0x7F));
        }
    } else if (cc == 32) {
        s_lsb[i] = val & 0x7F;
    }
}

extern "C" void kit_change_program(uint8_t ch, uint8_t program)
{
    if (ch < 1 || ch > 16 || program > 127) return;
    ++s_seen;
    if (mode_now() == crosspad::kitpc::Mode::Off) {
        ++s_ignored;
        return;
    }
    const uint32_t packed = kValidBit | (static_cast<uint32_t>(ch) << 24) |
                            (static_cast<uint32_t>(s_msb[ch - 1]) << 16) |
                            (static_cast<uint32_t>(s_lsb[ch - 1]) << 8) | program;
    s_parked.store(packed);
    /* One node in the async queue at a time: the newest request is the only one
     * that matters, which is also what the kit loader itself does with a burst
     * (docs/subsystems/kit-swap.md:29-35). */
    if (!s_posted.exchange(true)) lv_async_call_locked(apply_parked, nullptr);
}

extern "C" void kit_change_pair(uint8_t ch1, uint8_t msb, uint8_t program)
{
    /* The same parking as a Program Change -- newest wins, one queued apply, and the
     * bank comes from the frame rather than from the per-channel latch, because a
     * SysEx surface write names both halves at once. Everything else (guards, the
     * announcement seeding, the refusal reason) is the shared apply path's job. */
    if (ch1 < 1 || ch1 > 16 || program > 127) return;
    ++s_seen;
    if (mode_now() == crosspad::kitpc::Mode::Off) {
        ++s_ignored;
        return;
    }
    const uint32_t packed = kValidBit | (static_cast<uint32_t>(ch1) << 24) |
                            (static_cast<uint32_t>(msb) << 16) |
                            (static_cast<uint32_t>(s_lsb[ch1 - 1]) << 8) | program;
    s_parked.store(packed);
    if (!s_posted.exchange(true)) lv_async_call_locked(apply_parked, nullptr);
}

extern "C" void kit_change_init(void)
{
    NvsSettingsStore &nvs = NvsSettingsStore::getInstance();
    const uint8_t m = nvs.readU8(kNs, kKeyMode, static_cast<uint8_t>(kDefaultMode));
    s_mode.store(m <= KIT_PC_GROUP ? static_cast<int>(m) : static_cast<int>(kDefaultMode));
    const uint8_t c = nvs.readU8(kNs, kKeyCh, 0);      /* 0 = any channel */
    s_channel.store(c >= 1 && c <= 16 ? static_cast<int>(c) - 1 : -1);
    s_publicCh.store(c <= 16 ? c : 0);
    ESP_LOGI(TAG, "program change selects kits: mode=%s ch=%d",
             kit_change_mode_name(static_cast<kit_pc_mode_t>(s_mode.load())),
             s_publicCh.load());
}

extern "C" kit_pc_mode_t kit_change_mode(void)
{
    return static_cast<kit_pc_mode_t>(s_mode.load());
}

extern "C" void kit_change_set_mode(kit_pc_mode_t mode)
{
    if (mode > KIT_PC_GROUP) mode = KIT_PC_OFF;
    s_mode.store(static_cast<int>(mode));
    NvsSettingsStore::getInstance().saveU8(kNs, kKeyMode, static_cast<uint8_t>(mode));
    ESP_LOGI(TAG, "program-change mode -> %s", kit_change_mode_name(mode));
}

extern "C" int kit_change_channel(void) { return s_publicCh.load(); }

extern "C" void kit_change_set_channel(int ch)
{
    if (ch < 0 || ch > 16) return;
    s_publicCh.store(ch);
    s_channel.store(ch >= 1 ? ch - 1 : -1);   /* the core API is 0-based */
    NvsSettingsStore::getInstance().saveU8(kNs, kKeyCh, static_cast<uint8_t>(ch));
    ESP_LOGI(TAG, "program-change channel -> %d", ch);
}

extern "C" const char *kit_change_mode_name(kit_pc_mode_t mode)
{
    return mode == KIT_PC_FLAT ? "flat" : mode == KIT_PC_GROUP ? "group" : "off";
}

extern "C" uint32_t kit_change_count(int which)
{
    switch (which) {
    case kLoaded:  return s_loaded.load();
    case kRefused: return s_refused.load();
    case kIgnored: return s_ignored.load();
    default:       return s_seen.load();
    }
}

extern "C" int kit_change_last_kit(void) { return s_lastKit.load(); }

extern "C" const char *kit_change_last_why(void)
{
    return crosspad::kitpc::whyName(static_cast<crosspad::kitpc::Why>(s_lastWhy.load()));
}

/* KIT_PC STATUS: the state, what it has done, and what it could reach. Written
 * on the main task like every other verb line; the kit manager is read the same
 * way KIT_LIST reads it, and a scan in progress answers the same way. */
extern "C" int kit_change_status_line(char *out, size_t len)
{
    if (!out || !len) return 0;
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    const kit_pc_mode_t m = static_cast<kit_pc_mode_t>(s_mode.load());
    const int n = snprintf(out, len,
                           "KITPC: mode=%s ch=%d seen=%u loaded=%u refused=%u ignored=%u "
                           "cur=%d last_kit=%d last=%s echo=%d announced=%d ann=%u kits=%d",
                           kit_change_mode_name(m), s_publicCh.load(),
                           (unsigned)s_seen.load(), (unsigned)s_loaded.load(),
                           (unsigned)s_refused.load(), (unsigned)s_ignored.load(),
                           mgr ? mgr->getCurrentKitId() : -1, s_lastKit.load(),
                           kit_change_last_why(), s_echo.load(), s_announced.load(),
                           (unsigned)s_announceN.load(), mgr ? mgr->getKitCount() : 0);
    if (n < 0 || static_cast<size_t>(n) >= len) return len ? (int)(len - 1) : 0;
    /* The reachable count is the number a host wants before it starts sending
     * programs into the top of the list: 121 kits on a card, 128 program
     * numbers, one block per bank MSB in flat mode. */
    const size_t reach = mgr ? crosspad::kitpc::addressable(ManagerListing{mgr},
                                                            mode_now())
                             : 0;
    snprintf(out + n, len - n, " reach=%u", (unsigned)reach);
    return static_cast<int>(strlen(out));
}

/* One row of the map, on its own line: 121 kits do not fit one reply, and a
 * reply the CDC FIFO splits into 128-byte pieces must not lose the middle of a
 * row. Names are quoted and bytes below 0x21 replaced, so one entry is one
 * line. Read on the main task, the way KIT_LIST reads the listing. */
extern "C" int kit_change_map_row(int position, char *out, size_t len)
{
    if (!out || !len) return 0;
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    if (!mgr || position < 0) {
        out[0] = '\0';
        return 0;
    }
    return crosspad::kitpc::formatRow(ManagerListing{mgr}, mode_now(),
                                      static_cast<size_t>(position), out, len)
               ? 1
               : 0;
}

extern "C" int kit_change_map_total(void)
{
    crosspad::IKitManager *mgr = crosspad::getKitManager();
    return mgr ? static_cast<int>(crosspad::kitpc::addressable(ManagerListing{mgr}, mode_now()))
               : 0;
}
