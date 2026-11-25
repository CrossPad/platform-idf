// SPDX-License-Identifier: GPL-3.0-or-later
// main/include/kit_pc.h: what a host's Program Change + Bank Select names.
// No board, no ESP, no LVGL: the whole point of the header is that this can be
// pinned here, under ASan, before it is ever tried on hardware.
#include "kit_pc.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace crosspad::kitpc;

namespace {

/// The listing, as the board hands it: an accessor over positions, nothing
/// copied. The test's vector is the same shape ManagerListing has over the kit
/// manager (main/midi_kit_pc.cpp).
struct VectorListing {
    const std::vector<Slot> *v;
    size_t count() const { return v->size(); }
    Slot at(size_t i) const { return (*v)[i]; }
};

VectorListing listingOf(const std::vector<Slot> &v) { return VectorListing{&v}; }

Slot slot(int id, Group g = Group::Perf, bool loadable = true, const char *name = nullptr)
{
    Slot s;
    s.id = id;
    s.group = g;
    s.loadable = loadable;
    s.name = name;
    return s;
}

std::vector<Slot> threeKits()
{
    return {slot(0, Group::Perf, true, "Alpha"), slot(1, Group::Drums, true, "Beta"),
            slot(2, Group::Perf, true, "Gamma")};
}

}  // namespace

TEST_CASE("flat: the program is the position in the listing")
{
    const auto kits = threeKits();
    auto d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 0);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 0);
    CHECK(d.index == 0);

    d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 2);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 2);

    d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 3);
    CHECK(d.why == Why::OutOfRange);
    CHECK(d.kitId == -1);
}

TEST_CASE("flat: the bank MSB opens the next block of 128")
{
    std::vector<Slot> kits;
    for (int i = 0; i < 200; ++i) kits.push_back(slot(i));
    auto d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 127);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 127);
    CHECK(d.index == 127);

    d = resolve(listingOf(kits), Mode::Flat, -1, 0, 1, 0, 5);
    CHECK(d.why == Why::Ok);
    CHECK(d.index == 133);
    CHECK(d.kitId == 133);

    d = resolve(listingOf(kits), Mode::Flat, -1, 0, 2, 0, 0);
    CHECK(d.why == Why::OutOfRange);   // 256 is past 200 kits
}

TEST_CASE("flat: a slot that is there but not a kit is refused, not stepped over")
{
    // A deleted kit keeps its id slot (core KitList.hpp:18-20): if the map
    // skipped it, every program above it would move up one and the host's
    // setlist would drift under its own feet.
    std::vector<Slot> kits = {slot(0, Group::Perf, true, "Alpha"),
                              slot(1, Group::Perf, false, ""),
                              slot(2, Group::Perf, true, "Gamma")};
    auto d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 1);
    CHECK(d.why == Why::NotLoadable);
    CHECK(d.kitId == 1);     // named in the log, never requested
    CHECK(d.index == 1);

    d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 2);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 2);     // still slot two: nothing shifted
}

TEST_CASE("group: the MSB is the kind, the program the place inside it")
{
    const auto kits = threeKits();   // perf, drums, perf
    auto d = resolve(listingOf(kits), Mode::Group, -1, 0, 0, 0, 0);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 0);            // first performance kit
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 0, 0, 1);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 2);            // second performance kit: Beta is not one
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 1, 0, 0);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 1);            // the drum kit
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 1, 0, 1);
    CHECK(d.why == Why::OutOfRange);
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 4, 0, 0);
    CHECK(d.why == Why::OutOfRange);   // no user kits on this card
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 5, 0, 0);
    CHECK(d.why == Why::OutOfRange);   // no such kind
}

TEST_CASE("group: a slot that is not loadable is not in the bucket at all")
{
    std::vector<Slot> kits = {slot(0, Group::Drums, true, "One"),
                              slot(1, Group::Drums, false, ""),
                              slot(2, Group::Drums, true, "Three")};
    auto d = resolve(listingOf(kits), Mode::Group, -1, 0, 1, 0, 1);
    CHECK(d.why == Why::Ok);
    CHECK(d.kitId == 2);   // the deleted one is not counted... 
    d = resolve(listingOf(kits), Mode::Group, -1, 0, 1, 0, 2);
    CHECK(d.why == Why::OutOfRange);   // ...and nothing beyond it exists
}

TEST_CASE("the channel filter and the off switch")
{
    const auto kits = threeKits();
    CHECK(resolve(listingOf(kits), Mode::Off, -1, 0, 0, 0, 0).why == Why::Off);
    CHECK(resolve(listingOf(kits), Mode::Flat, 2, 2, 0, 0, 0).why == Why::Ok);
    CHECK(resolve(listingOf(kits), Mode::Flat, 2, 3, 0, 0, 0).why == Why::Channel);
    CHECK(resolve(listingOf(kits), Mode::Flat, -1, 15, 0, 0, 0).why == Why::Ok);
    const auto empty = std::vector<Slot>{};
    CHECK(resolve(listingOf(empty), Mode::Flat, -1, 0, 0, 0, 0).why == Why::Empty);
}

TEST_CASE("addressable says what a host could reach, so silence is explainable")
{
    const auto kits = threeKits();
    CHECK(addressable(listingOf(kits), Mode::Flat) == 3);
    CHECK(addressable(listingOf(kits), Mode::Group) == 3);
    std::vector<Slot> holes = {slot(0, Group::Perf, false, ""), slot(1, Group::Perf, true, "x")};
    CHECK(addressable(listingOf(holes), Mode::Flat) == 2);    // positions exist
    CHECK(addressable(listingOf(holes), Mode::Group) == 1);   // only loadable ones
    CHECK(addressable(listingOf(kits), Mode::Off) == 0);
}

TEST_CASE("the map rows name the exact pair a host has to send")
{
    std::vector<Slot> many;
    for (int i = 0; i < 200; ++i) many.push_back(slot(i));
    uint8_t msb = 9, pc = 9;
    CHECK(pairAt(listingOf(many), Mode::Flat, 0, &msb, &pc));
    CHECK(msb == 0); CHECK(pc == 0);
    CHECK(pairAt(listingOf(many), Mode::Flat, 127, &msb, &pc));
    CHECK(msb == 0); CHECK(pc == 127);
    CHECK(pairAt(listingOf(many), Mode::Flat, 130, &msb, &pc));   // second bank block
    CHECK(msb == 1); CHECK(pc == 2);
    CHECK_FALSE(pairAt(listingOf(many), Mode::Flat, 200, &msb, &pc));

    // GROUP: the position runs kind by kind, and the pair restarts inside each.
    std::vector<Slot> kits = {slot(0, Group::Perf, true, "P1"), slot(1, Group::Drums, true, "D1"),
                              slot(2, Group::Perf, true, "P2"), slot(3, Group::Drums, true, "D2")};
    CHECK(pairAt(listingOf(kits), Mode::Group, 0, &msb, &pc));
    CHECK(msb == 0); CHECK(pc == 0);            // first performance kit
    CHECK(pairAt(listingOf(kits), Mode::Group, 1, &msb, &pc));
    CHECK(msb == 0); CHECK(pc == 1);            // second performance kit
    CHECK(pairAt(listingOf(kits), Mode::Group, 2, &msb, &pc));
    CHECK(msb == 1); CHECK(pc == 0);            // first drum kit, program restarts
    CHECK(pairAt(listingOf(kits), Mode::Group, 3, &msb, &pc));
    CHECK(msb == 1); CHECK(pc == 1);
    CHECK_FALSE(pairAt(listingOf(kits), Mode::Group, 4, &msb, &pc));
    CHECK(addressable(listingOf(kits), Mode::Group) == 4);

    // The pair the map prints is the pair resolve() takes: what a host reads is
    // what a host sends, in both directions.
    for (size_t p = 0; p < 4; ++p) {
        if (!pairAt(listingOf(kits), Mode::Group, p, &msb, &pc)) continue;
        const Decision d = resolve(listingOf(kits), Mode::Group, -1, 0, msb, 0, pc);
        CHECK(d.why == Why::Ok);
        CHECK(slotAt(listingOf(kits), Mode::Group, p).id == d.kitId);
    }
}

TEST_CASE("a map row is one line, sanitised, and never half-written")
{
    std::vector<Slot> kits = {slot(0, Group::Perf, true, "Alpha"),
                              slot(1, Group::Perf, false, ""),
                              slot(2, Group::Perf, true, "Bad\nname\"x"),
                              slot(3, Group::Drums, true,
                                   "A name far too long for one line of the map reply")};
    char line[128];
    CHECK(formatRow(listingOf(kits), Mode::Flat, 0, line, sizeof(line)));
    CHECK(std::string(line) == "KITPCMAP: msb=0 pc=0 id=0 kit \"Alpha\"");

    // A hole is shown as what it is, refused by name rather than stepped over.
    CHECK(formatRow(listingOf(kits), Mode::Flat, 1, line, sizeof(line)));
    CHECK(std::string(line).find(" id=1 empty \"\"!") != std::string::npos);

    // A name a person typed cannot break the framing, and a long name is cut.
    CHECK(formatRow(listingOf(kits), Mode::Flat, 2, line, sizeof(line)));
    CHECK(std::strchr(line, '\n') == nullptr);
    CHECK(std::string(line).find("\"Bad name x\"") != std::string::npos);
    CHECK(formatRow(listingOf(kits), Mode::Flat, 3, line, sizeof(line)));
    CHECK(std::strlen(line) < sizeof(line));
    CHECK(std::string(line).find("A name far too long for one line o") != std::string::npos);

    // Past the end and a buffer that cannot hold the line: no output, and the
    // caller is told -- nothing half-written reaches the wire.
    CHECK_FALSE(formatRow(listingOf(kits), Mode::Flat, 9, line, sizeof(line)));
    CHECK(line[0] == '\0');
    char small[8];
    small[0] = 'x';
    CHECK_FALSE(formatRow(listingOf(kits), Mode::Flat, 0, small, sizeof(small)));
    CHECK(small[0] == '\0');

    // GROUP says which kind a row is, so the bank it names is readable.
    std::vector<Slot> g = {slot(7, Group::Drums, true, "KitKat")};
    CHECK(formatRow(listingOf(g), Mode::Group, 0, line, sizeof(line)));
    CHECK(std::string(line) == "KITPCMAP: msb=1 pc=0 id=7 drums \"KitKat\"");
}

TEST_CASE("ids are not assumed to be 0..n-1")
{
    // A board that ever drops slots out of its listing keeps working: the
    // decision names the id the manager gave, not the position.
    std::vector<Slot> kits = {slot(7), slot(11), slot(23)};
    auto d = resolve(listingOf(kits), Mode::Flat, -1, 0, 0, 0, 1);
    CHECK(d.why == Why::Ok);
    CHECK(d.index == 1);
    CHECK(d.kitId == 11);
}

TEST_CASE("what the board announces about a kit is the addressing the map shows for it")
{
    /* The announcing half of the feature starts from one kit id and has to answer
     * with the bank/program pair a host would have sent to get that kit. FLAT and
     * GROUP count positions differently on purpose, so `positionOf` must walk the
     * ordering `resolve` walks -- otherwise a DAW is told one thing by `KIT_PC MAP`
     * and another by the board, and a setlist drifts against itself.
     *
     * The listing below is built to tell the two orderings apart: a deleted kit in
     * the middle (a hole FLAT counts and GROUP does not), and kits of three kinds
     * out of order. */
    const std::vector<Slot> mixed = {
        slot(10, Group::Perf, true, "Live Set"),
        slot(11, Group::Drums, true, "Kit 808"),
        slot(12, Group::Perf, false, ""),
        slot(13, Group::Slice, true, "Breaks"),
        slot(14, Group::User, true, "My pads"),
    };
    for (int mode = 1; mode <= 2; ++mode) {
        const Mode m = static_cast<Mode>(mode);
        const auto L = listingOf(mixed);
        for (size_t i = 0; i < mixed.size(); ++i) {
            if (!mixed[i].loadable) continue;
            const int pos = positionOf(L, m, mixed[i].id);
            REQUIRE(pos >= 0);
            uint8_t msb = 0, pc = 0;
            REQUIRE(pairAt(L, m, static_cast<size_t>(pos), &msb, &pc));
            const Decision d = resolve(L, m, -1, 0, msb, 0, pc);
            CHECK(d.why == Why::Ok);
            CHECK(d.kitId == mixed[i].id);
            /* Neither mode reads the bank LSB, which is why an announcement does
             * not send CC 32 at all: nothing in the addressing depends on it. */
            CHECK(resolve(L, m, -1, 0, msb, 0x7F, pc).kitId == mixed[i].id);
        }
    }
    /* The ordering difference in two lines: kit 13 is the fourth slot of the
     * listing (FLAT, with the hole standing where it was) and the third kit of its
     * own kind in the group walk (GROUP, which never counts a hole). */
    CHECK(positionOf(listingOf(mixed), Mode::Flat, 13) == 3);
    CHECK(positionOf(listingOf(mixed), Mode::Group, 13) == 2);
    CHECK(positionOf(listingOf(mixed), Mode::Flat, 14) == 4);
    CHECK(positionOf(listingOf(mixed), Mode::Group, 14) == 3);

    /* Nothing to announce: an id the card does not hold, no id at all, a mode that
     * does not use programs. The caller remembers those as announced rather than
     * retrying them ten times a second, which is what makes the answer safe and not
     * merely correct. */
    const auto L = listingOf(mixed);
    CHECK(positionOf(L, Mode::Flat, 99999) == -1);
    CHECK(positionOf(L, Mode::Group, 99999) == -1);
    CHECK(positionOf(L, Mode::Off, 10) == -1);
    CHECK(positionOf(L, Mode::Flat, -1) == -1);
}
