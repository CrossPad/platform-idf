// Host tests for the CDC line assembler (main/include/cdc_line.h).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "cdc_line.h"

namespace {

// What the drain loop would do with a stream: dispatched lines, and
// "<too_long>" for every over-long line that was answered and dropped.
std::vector<std::string> run(CdcLineAssembler &a, const std::string &bytes)
{
    std::vector<std::string> out;
    for (char c : bytes) {
        switch (a.feed(c)) {
        case CdcLineAssembler::Result::Line:
            out.emplace_back(a.text(), a.size());
            break;
        case CdcLineAssembler::Result::TooLong:
            out.emplace_back("<too_long>");
            break;
        case CdcLineAssembler::Result::None:
            break;
        }
    }
    return out;
}

const size_t kMaxLine = CDC_LINE_MAX - 1;

}  // namespace

TEST_CASE("lines end at CR or LF, blank lines vanish")
{
    CdcLineAssembler a;
    CHECK(run(a, "PING\r\nKIT_STATUS\n\n\rFS_ROOTS\r") ==
          std::vector<std::string>{"PING", "KIT_STATUS", "FS_ROOTS"});
}

TEST_CASE("a partial line waits for its terminator across feeds")
{
    CdcLineAssembler a;
    CHECK(run(a, "FS_MKDIR /sdcard/compan").empty());
    CHECK(run(a, "ion_sync\n") == std::vector<std::string>{"FS_MKDIR /sdcard/companion_sync"});
}

TEST_CASE("a line of exactly the maximum length is dispatched whole")
{
    CdcLineAssembler a;
    const std::string line(kMaxLine, 'x');
    const auto got = run(a, line + "\n");
    REQUIRE(got.size() == 1);
    CHECK(got[0] == line);
}

TEST_CASE("an over-long line is dropped whole: no head, no tail, one TooLong")
{
    CdcLineAssembler a;
    // The tail is a real verb: it must not run.
    const std::string line = "FS_GET 0 0 /sdcard/" + std::string(kMaxLine, 'k') + "REBOOT";
    CHECK(run(a, line + "\n") == std::vector<std::string>{"<too_long>"});
    // The next line is untouched.
    CHECK(run(a, "PING\n") == std::vector<std::string>{"PING"});
}

TEST_CASE("over-long by one byte, and CRLF after it, answers once")
{
    CdcLineAssembler a;
    CHECK(run(a, std::string(kMaxLine + 1, 'y') + "\r\nPING\r\n") ==
          std::vector<std::string>{"<too_long>", "PING"});
}

TEST_CASE("an over-long line split across many feeds is still dropped whole")
{
    CdcLineAssembler a;
    const std::string line(3 * CDC_LINE_MAX, 'z');
    for (size_t off = 0; off < line.size(); off += 64)
        CHECK(run(a, line.substr(off, 64)).empty());
    CHECK(run(a, "\nPING\n") == std::vector<std::string>{"<too_long>", "PING"});
}

TEST_CASE("reset() drops a partial line and a pending discard")
{
    CdcLineAssembler a;
    CHECK(run(a, std::string(CDC_LINE_MAX + 10, 'q')).empty());
    a.reset();   // the stream became payload; it ended; text again
    CHECK(run(a, "PING\n") == std::vector<std::string>{"PING"});

    CHECK(run(a, "FS_PU").empty());
    a.reset();
    CHECK(run(a, "PING\n") == std::vector<std::string>{"PING"});
}

TEST_CASE("the longest FS_GET with a 159-character path fits")
{
    CdcLineAssembler a;
    const std::string line = "FS_GET 4294967295 4294967295 /sdcard/" + std::string(159 - 8, 'p');
    REQUIRE(line.size() <= kMaxLine);
    const auto got = run(a, line + "\n");
    REQUIRE(got.size() == 1);
    CHECK(got[0] == line);
}
