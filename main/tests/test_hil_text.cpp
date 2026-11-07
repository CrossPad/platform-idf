// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden lines of the CDC verbs whose state crosspad-core gathers. Every
// expected string here is what the board printed before the verb moved to
// crosspad/control/ -- crosspad-hil parses exactly these, so a change to one
// is a protocol change, not a refactor.
#include <catch2/catch_test_macros.hpp>

#include "hil_text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace crosspad::control;

TEST_CASE("KIT_STATUS line", "[hil_text]") {
    char out[160];
    KitStatus st;
    st.count = 3;
    st.current = 2;
    st.name = "808 Kit";
    st.loading = true;
    st.pending = 5;
    hil_text_kit_status(out, sizeof(out), st);
    CHECK(std::string(out) == "KITSTATUS: current=2 loading=1 pending=5 name=808 Kit\r\n");

    hil_text_kit_status(out, sizeof(out), KitStatus{});
    CHECK(std::string(out) == "KITSTATUS: current=-1 loading=0 pending=-1 name=-\r\n");
}

TEST_CASE("LED_STATE line", "[hil_text]") {
    LedState st;
    st.brightness = 100;
    st.coalesced = true;
    st.settingsBrightness = 80;
    st.colors[0] = crosspad::RgbColor(0xFF, 0x00, 0x00);
    st.colors[15] = crosspad::RgbColor(0x00, 0xFF, 0x80);
    const HilLedBoard board{0x03, 7, 0, 1234};
    char out[512];
    hil_text_led_state(out, sizeof(out), st, board);
    CHECK(std::string(out) ==
          "LEDS: bri=100 anim=0 coalesce=1 cfgbri=80 pwr=0x03 pwrN=7 txfail=0 refreshes=1234 "
          "colors=FF0000,000000,000000,000000,000000,000000,000000,000000,000000,000000,000000,"
          "000000,000000,000000,000000,00FF80\r\n");
}

TEST_CASE("MIX_LVL lines", "[hil_text]") {
    char out[176];
    MixerChannelLevel ch;
    ch.ch = 0;
    ch.name = "Sampler";
    ch.peakL = 0.5f;
    ch.peakR = 0.25f;
    ch.volume = 1.0f;
    ch.trim = 1.0f;
    ch.outputs = 4;
    ch.routes = 0b0101;
    hil_text_mix_channel(out, sizeof(out), ch);
    CHECK(std::string(out) == "MIXLVL: ch=0 name=Sampler pk=0.5000/0.2500 vol=1.00 trim=1.00 mute=0 route=1010\r\n");

    // Rounded, not truncated: 0.99996 is 1.0000, 0.755 is 0.76.
    ch.ch = 3;
    ch.name = "Mic";
    ch.peakL = 0.99996f;
    ch.peakR = 0.00004f;
    ch.volume = 0.755f;
    ch.trim = 0.505f;
    ch.muted = true;
    ch.outputs = 8;
    ch.routes = 0xFF;
    hil_text_mix_channel(out, sizeof(out), ch);
    CHECK(std::string(out) == "MIXLVL: ch=3 name=Mic pk=1.0000/0.0000 vol=0.76 trim=0.51 mute=1 route=11111111\r\n");

    MixerOutputLevel o;
    o.out = 1;
    o.peakL = 0.125f;
    o.volume = 0.5f;
    o.muted = true;
    char line[144];
    hil_text_mix_output(line, sizeof(line), o);
    CHECK(std::string(line) == "MIXLVL: out=1 pk=0.1250/0.0000 vol=0.50 mute=1 lim=1.000 ceil=1.000\r\n");

    o.out = 2;
    o.peakL = 0.0f;
    o.volume = 1.0f;
    o.muted = false;
    o.limit = 0.8125f;
    o.ceiling = 0.8913f;
    hil_text_mix_output(line, sizeof(line), o);
    CHECK(std::string(line) == "MIXLVL: out=2 pk=0.0000/0.0000 vol=1.00 mute=0 lim=0.813 ceil=0.891\r\n");

    char tapLine[48];
    MixerTapLevel tap;
    hil_text_mix_tap(tapLine, sizeof(tapLine), tap);
    CHECK(std::string(tapLine) == "MIXLVL: tap lim=1.000\r\n");
    tap.limit = 0.25f;
    hil_text_mix_tap(tapLine, sizeof(tapLine), tap);
    CHECK(std::string(tapLine) == "MIXLVL: tap lim=0.250\r\n");
}

TEST_CASE("PITCHED_STATUS line, healthy and after a failed load", "[hil_text]") {
    static char out[kHilPitchedLineChars];
    PitchedStatus st;
    st.active = true;
    st.zones = 2;
    st.roots[0] = 48.0f;
    st.roots[1] = 60.5f;
    st.activeVoices = 3;
    st.maxVoices = 16;
    st.bankBytes = 1234567;
    st.steals = 4;
    st.voiceCap = 28;
    st.sheds = 1;
    hil_text_pitched_status(out, sizeof(out), st, HilPitchedBoard{88, "", 900, 2, 24});
    CHECK(std::string(out) ==
          "PITCHED: active=1 zones=2 roots=48.00,60.50 voices=3/16 bank_kb=1205 rt_max_us=88 "
          "steals=4 rt_busy_max_us=900 overruns=2 cap=28 kit_max=24 shed=1\r\n");

    // err= only after a failed load, right after steals=.
    hil_text_pitched_status(out, sizeof(out), PitchedStatus{}, HilPitchedBoard{0, "zone_too_long", 0, 0, 0});
    CHECK(std::string(out) ==
          "PITCHED: active=0 zones=0 roots= voices=0/0 bank_kb=0 rt_max_us=0 steals=0 "
          "err=zone_too_long rt_busy_max_us=0 overruns=0 cap=0 kit_max=0 shed=0\r\n");
}

TEST_CASE("PITCHED_STATUS roots field keeps its old bound", "[hil_text]") {
    static char out[kHilPitchedLineChars];
    PitchedStatus st;
    st.zones = crosspad::SampleBank::kMaxZones;
    for (auto& r : st.roots) r = -1234.5f;   // "-1234.50," a root: the field fills before the zones end
    hil_text_pitched_status(out, sizeof(out), st, HilPitchedBoard{0, "", 0, 0, 0});
    const std::string line(out);
    const size_t from = line.find("roots=") + std::strlen("roots=");
    const size_t to = line.find(" voices=");
    REQUIRE(to != std::string::npos);
    CHECK(to - from < kHilPitchedRootsChars);
    CHECK(line.compare(line.size() - 2, 2, "\r\n") == 0);
}

TEST_CASE("WAVE_STATUS line", "[hil_text]") {
    static char out[1024];
    WaveStatus st;
    st.kit = 4;
    st.total = 12;
    st.cached = 10;
    st.loaderRunning = true;
    st.requested = 1;
    st.dropped = 2;
    st.loaded = 3;
    st.retried = 4;
    st.failedInfo = 5;
    st.failedRead = 6;
    st.allocFail = 7;
    st.sweeps = 8;
    st.startFail = 9;
    st.queued = 10;
    std::strcpy(st.missing, "0:2,5:0");
    hil_text_wave_status(out, sizeof(out), st);
    CHECK(std::string(out) ==
          "WAVE: kit=4 total=12 cached=10 loader=1 queued=10 req=1 ok=3 retry=4 fail_info=5 "
          "fail_read=6 alloc_fail=7 drop=2 sweep=8 start_fail=9 missing=0:2,5:0\r\n");

    hil_text_wave_status(out, sizeof(out), WaveStatus{});
    CHECK(std::string(out) ==
          "WAVE: kit=-1 total=0 cached=0 loader=0 queued=0 req=0 ok=0 retry=0 fail_info=0 "
          "fail_read=0 alloc_fail=0 drop=0 sweep=0 start_fail=0 missing=-\r\n");
}

namespace {
std::string g_lines;
void collect(const char* line) { g_lines += line; }

std::string kitList(const std::vector<KitListRow>& rows, int current) {
    g_lines.clear();
    static char out[512];
    HilKitListText text{out, sizeof(out), 0, 0, collect};
    hil_text_kit_list_begin(text);
    for (const auto& row : rows) hil_text_kit_list_add(text, row);
    hil_text_kit_list_end(text, (int)rows.size(), current);
    return g_lines;
}
}

TEST_CASE("KIT_LIST lines leave removed kits out", "[hil_text]") {
    KitListRow alpha;
    alpha.id = 0;
    alpha.name = "Alpha";
    KitListRow gone;
    gone.id = 1;
    gone.name = "gone";
    gone.removed = true;
    KitListRow missing;   // the manager had no kit at this id
    missing.id = 2;
    CHECK(kitList({alpha, gone, missing}, 0) == "KITS: 0:Alpha,2:?\r\nKITS: end count=3 current=0\r\n");
    CHECK(kitList({}, -1) == "KITS: end count=0 current=-1\r\n");
}

TEST_CASE("KIT_LIST lines break before a name that would not fit", "[hil_text]") {
    std::vector<std::string> names;
    std::vector<KitListRow> rows;
    for (int i = 0; i < 30; ++i) {
        char n[32];
        snprintf(n, sizeof(n), "Kit number %02d padded", i);
        names.push_back(n);
    }
    for (int i = 0; i < 30; ++i) {
        KitListRow row;
        row.id = i;
        row.name = names[i].c_str();
        rows.push_back(row);
    }
    std::string first = "KITS:";
    for (int i = 0; i <= 20; ++i) first += (i ? "," : " ") + std::to_string(i) + ":" + names[i];
    std::string second = "KITS:";
    for (int i = 21; i < 30; ++i) second += (i > 21 ? "," : " ") + std::to_string(i) + ":" + names[i];
    CHECK(kitList(rows, 7) == first + "\r\n" + second + "\r\nKITS: end count=30 current=7\r\n");
}

TEST_CASE("AUDIO_LEVEL line", "[hil_text]") {
    char out[128];
    AudioLevel lv;
    lv.left = 0.5f;
    hil_text_audio_level(out, sizeof(out), lv, true, false);
    CHECK(std::string(out) == "AUDIOLVL: L=0.50000 R=0.00000 amp=1 allowed=0\r\n");
}

namespace {
/// Room each sweep tries: every buffer size from nothing to past the longest line.
constexpr size_t kSweepCap = 1200;
const std::string kHuge(2000, 'x');
/// Long enough for "KITS: end count=40 current=0\r\n".
constexpr size_t kKitListEndLineChars = 64;

/// Runs @p format into a heap buffer of exactly @p cap bytes, so a byte past
/// it is AddressSanitizer's; checks the line is terminated inside it.
template <class Format>
void sweep(Format&& format) {
    for (size_t cap = 1; cap <= kSweepCap; ++cap) {
        std::vector<char> buf(cap);
        const size_t n = format(buf.data(), cap);
        REQUIRE(n < cap);
        REQUIRE(std::strlen(buf.data()) == n);
    }
}
}

TEST_CASE("every formatter stays inside its buffer, however long the values", "[hil_text][bounds]") {
    KitStatus kit;
    kit.name = kHuge.c_str();
    kit.current = -2147483647 - 1;
    kit.pending = 2147483647;
    sweep([&](char* out, size_t cap) { return hil_text_kit_status(out, cap, kit); });

    LedState led;
    for (auto& c : led.colors) c = crosspad::RgbColor(0xFF, 0xFF, 0xFF);
    const HilLedBoard board{0xFF, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    sweep([&](char* out, size_t cap) { return hil_text_led_state(out, cap, led, board); });

    MixerChannelLevel ch;
    ch.name = kHuge.c_str();
    ch.outputs = 255;           // past the route mask's width
    ch.routes = 0xFFFFFFFFu;
    ch.peakL = ch.peakR = ch.volume = ch.trim = 100000.0f;
    sweep([&](char* out, size_t cap) { return hil_text_mix_channel(out, cap, ch); });
    MixerOutputLevel o;
    o.out = 255;
    o.peakL = o.peakR = o.volume = o.limit = o.ceiling = 100000.0f;
    sweep([&](char* out, size_t cap) { return hil_text_mix_output(out, cap, o); });
    MixerTapLevel tap;
    tap.limit = 100000.0f;
    sweep([&](char* out, size_t cap) { return hil_text_mix_tap(out, cap, tap); });

    PitchedStatus pit;
    pit.zones = 255;            // past kMaxZones: roots[] must not be read past its end
    for (auto& r : pit.roots) r = -1.0e7f;
    pit.bankBytes = (size_t)-1;
    const HilPitchedBoard pboard{0xFFFFFFFFu, kHuge.c_str(), 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF};
    sweep([&](char* out, size_t cap) { return hil_text_pitched_status(out, cap, pit, pboard); });

    static WaveStatus wave;
    std::memset(wave.missing, '9', sizeof(wave.missing) - 1);
    wave.missing[sizeof(wave.missing) - 1] = '\0';
    sweep([&](char* out, size_t cap) { return hil_text_wave_status(out, cap, wave); });

    AudioLevel lv;
    lv.left = lv.right = 3.4e38f;
    sweep([&](char* out, size_t cap) { return hil_text_audio_level(out, cap, lv, true, true); });
}

TEST_CASE("KIT_LIST cuts a name too long for a line and keeps every line ended", "[hil_text][bounds]") {
    std::vector<KitListRow> rows;
    for (int i = 0; i < 40; ++i) {
        KitListRow row;
        row.id = i;
        row.name = (i % 3 == 0) ? kHuge.c_str() : "Kit";
        rows.push_back(row);
    }
    for (size_t cap : {size_t{8}, size_t{32}, size_t{64}, size_t{511}, size_t{512}, size_t{513}, size_t{2100}}) {
        g_lines.clear();
        std::vector<char> buf(cap);
        HilKitListText text{buf.data(), cap, 0, 0, collect};
        hil_text_kit_list_begin(text);
        for (const auto& row : rows) hil_text_kit_list_add(text, row);
        hil_text_kit_list_end(text, (int)rows.size(), 0);
        if (cap < kKitListEndLineChars) continue;   // bounds only: the end line itself does not fit
        size_t from = 0;
        while (from < g_lines.size()) {
            const size_t end = g_lines.find("\r\n", from);
            REQUIRE(end != std::string::npos);
            CHECK(end + 2 - from < cap);
            from = end + 2;
        }
    }
}
