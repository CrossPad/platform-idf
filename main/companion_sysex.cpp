/**
 * @file companion_sysex.cpp
 * @brief Companion control over USB MIDI SysEx — see companion_sysex.h.
 *
 * Same bounded mailbox and allowlist as the BLE front. Threads: the MIDI
 * task's RX callback fills the mailbox, the main loop executes the verb
 * (companion_sysex_poll), the MIDI task sends the reply (companion_sysex_pump)
 * before it reads more RX — so USB MIDI TX keeps one writer, and a finished
 * reply always leaves before the next request can overwrite it.
 */
#include "companion_sysex.h"
#include "BleControlMailbox.hpp"
#include "hil_control.h"
#include "motion_stream.h"
#include "twin_mirror.h"
#include "crosspad/protocol/SysExProtocol.h"
#include <atomic>
#include <cstring>

namespace {
constexpr uint8_t kSubRequest = 0x01;
constexpr uint8_t kSubReplyMore = 0x02;
constexpr uint8_t kSubReplyLast = 0x03;
constexpr uint8_t kSubNotify = 0x04;
constexpr size_t kMaxRequest = 190;    // BleControlMailbox::command_ minus LF and NUL
constexpr size_t kChunkPayload = 200;  // 4-byte header + payload + F7 stays under the 256 SysExMaxSize
constexpr size_t kHeaderSize = 4;      // F0 7D 1F <sub>

crosspad::BleControlMailbox s_mailbox;
companion_sysex_sender_t s_send = nullptr;
void (*s_wake)() = nullptr;
/* The verb in the mailbox came as a notify: its reply is dropped, not sent.
 * Every port listener sees every reply, so a periodic sender (the MIDI hub's
 * HOST_INFO) must not put one where a browser's request is waiting for its own. */
std::atomic<bool> s_quiet{false};

void send_reply(const char *text)
{
    if (!s_send) return;
    static uint8_t frame[kHeaderSize + kChunkPayload + 1];
    size_t left = std::strlen(text);
    do {
        const size_t n = left < kChunkPayload ? left : kChunkPayload;
        frame[0] = CROSSPAD_SYSEX_START;
        frame[1] = CROSSPAD_SYSEX_MANUFACTURER;
        frame[2] = CROSSPAD_CMD_COMPANION;
        frame[3] = (n == left) ? kSubReplyLast : kSubReplyMore;
        for (size_t i = 0; i < n; ++i) {
            // SysEx data is 7-bit: a byte with the MSB set would end the frame early.
            const uint8_t c = static_cast<uint8_t>(text[i]);
            frame[kHeaderSize + i] = c < 0x80 ? c : '?';
        }
        frame[kHeaderSize + n] = CROSSPAD_SYSEX_END;
        s_send(frame, kHeaderSize + n + 1);
        text += n;
        left -= n;
    } while (left > 0);
}
} // namespace

void companion_sysex_init(companion_sysex_sender_t send) { s_send = send; }

void companion_sysex_set_wake(void (*wake)(void)) { s_wake = wake; }

void companion_sysex_receive(const uint8_t *args, unsigned len)
{
    if (!s_send || len < 2 || (args[0] != kSubRequest && args[0] != kSubNotify)) return;
    // A notify is never answered, not even with an error: a busy mailbox drops it.
    const bool quiet = args[0] == kSubNotify;
    const size_t n = len - 1;
    /* The keepalives (TWIN 1, MOTION 1) only move a deadline: done here, on the
     * MIDI task, not through the mailbox. A page that streams the twin keeps the
     * mailbox busy with its own requests (TWIN_STATE every second), and a notify
     * finding it busy is dropped -- the motion stream stopped 6 s after a twin
     * page opened, every time. */
    if (quiet && n == 8 && std::memcmp(args + 1, "MOTION ", 7) == 0) { motion_stream_enable(args[8] == '1'); return; }
    if (quiet && n == 6 && std::memcmp(args + 1, "TWIN ", 5) == 0) { twin_mirror_enable(args[6] == '1'); return; }
    if (n > kMaxRequest) { if (!quiet) send_reply("ERR request_too_long\r\n"); return; }
    uint8_t line[kMaxRequest + 1];
    std::memcpy(line, args + 1, n);
    line[n] = '\n';
    if (s_mailbox.busy()) { if (!quiet) send_reply("ERR busy\r\n"); return; }
    s_quiet.store(quiet);
    if (!s_mailbox.write(line, n + 1)) {
        s_quiet.store(false);
        if (!quiet) send_reply("ERR bad_request\r\n");
        return;
    }
    if (s_wake) s_wake();
}

void companion_sysex_poll(void)
{
    // Static: the main task has 5 kB of stack (see bleCompanionPoll).
    static char command[192];
    static char reply[481];
    uint32_t generation;
    if (!s_mailbox.take(command, generation)) return;
    reply[0] = 0;
    hil_control_companion_handle(command, reply, sizeof(reply));
    s_mailbox.finish(reply, generation);
}

void companion_sysex_pump(void)
{
    static uint8_t response[482];
    if (!s_mailbox.ready()) return;
    const size_t size = s_mailbox.read(response);  // [0] = 1, then the text without NUL
    s_mailbox.reset();
    if (s_quiet.exchange(false)) return;
    response[size] = 0;
    send_reply(reinterpret_cast<const char *>(response + 1));
}
