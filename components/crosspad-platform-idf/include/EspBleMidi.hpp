#pragma once

/**
 * @file EspBleMidi.hpp
 * @brief ESP-IDF (NimBLE) implementation of crosspad-core's IBleMidi.
 *
 * Both roles of the BLE MIDI spec are implemented:
 *  - Server (peripheral): advertises the MIDI service; a host (PC, phone,
 *    tablet) connects and receives notifications. This is what a pad
 *    controller normally is.
 *  - Host (central): scans for BLE MIDI peripherals, connects, subscribes.
 *
 * The stack is started by begin() and fully torn down by end(), so a board
 * that never enables BLE pays no RAM for it.
 */

#include "crosspad/midi/IBleMidi.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace crosspad {

class EspBleMidi : public IBleMidi {
public:
    /// Counters for HIL/diagnostics — cheap enough to keep always on.
    struct Stats {
        uint32_t txMessages = 0;  ///< MIDI messages accepted for transmission
        uint32_t txPackets  = 0;  ///< BLE notifications/writes actually sent
        uint32_t txDropped  = 0;  ///< messages dropped (queue full / not connected)
        uint32_t txErrors   = 0;  ///< BLE stack refused the write
        uint32_t rxMessages = 0;  ///< MIDI messages parsed out of BLE packets
        uint32_t rxPackets  = 0;  ///< BLE packets received
    };

    static EspBleMidi& instance();

    // ── IBleMidi lifecycle ──────────────────────────────────────────────
    bool begin(BleMidiMode mode) override;
    void end() override;
    bool isConnected() const override;
    BleMidiMode getMode() const override;

    // ── Host mode (central) ─────────────────────────────────────────────
    void startScan(uint16_t durationMs = 5000) override;
    void stopScan() override;
    bool isScanning() const override;
    std::vector<BleMidiDevice> getScanResults() const override;
    bool connectToDevice(const std::string& address) override;

    // ── Server mode (peripheral) ────────────────────────────────────────
    void startAdvertising(const std::string& deviceName = "CrossPad") override;
    void stopAdvertising() override;
    bool isAdvertising() const override;

    // ── Common ──────────────────────────────────────────────────────────
    void disconnect() override;
    BleMidiDevice getConnectedDevice() const override;

    // ── IMidiOutput ─────────────────────────────────────────────────────
    void sendNoteOn(uint8_t note, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override;
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override;
    void sendProgramChange(uint8_t program, uint8_t channel) override;
    void sendPitchBend(int16_t value, uint8_t channel) override;
    /// One MIDI realtime byte (0xF8..0xFF). The BLE timestamp is `atUs`
    /// (esp_timer time of the tick), not the moment the packet is built, so a
    /// receiver that honours timestamps undoes the connection-interval jitter.
    void sendRealTime(uint8_t status, int64_t atUs);

    /**
     * @brief Decode one BLE MIDI packet and fire the input callbacks.
     *
     * Called by the NimBLE write/notify handlers. Public so a HIL command can
     * inject a packet without a radio link.
     */
    void feedPacket(const uint8_t* data, size_t len);

    /// Re-publish IBleMidi's protected dispatchers for the NimBLE GAP callbacks.
    void notifyConnectionChanged(bool connected, const BleMidiDevice& device);
    void notifyScanResults(const std::vector<BleMidiDevice>& devices);

    // ── Diagnostics ─────────────────────────────────────────────────────
    bool isRunning() const;
    Stats getStats() const;
    void resetStats();
    /// Negotiated connection interval in units of 1.25 ms (0 = not connected).
    uint16_t getConnInterval() const;
    /// Peer name/address plus connection state, for status readouts.
    const char* getStateName() const;

    /// This radio's own identity address, formatted as a scan would show
    /// it. Empty while the stack is stopped.
    ///
    /// A host-side test cannot otherwise tell which peripheral is this
    /// board: the BLE MIDI service UUID is shared by every MIDI device in
    /// range, and the name is shared by every CrossPad on hardware.
    std::string getOwnAddress() const;
    /// True when the stack was compiled in (CONFIG_BT_NIMBLE_ENABLED).
    static bool isSupported();

private:
    EspBleMidi() = default;

    /// One decoded channel message → the IBleMidi input callbacks.
    void dispatchParsed(uint8_t status, uint8_t d0, uint8_t d1);
};

/// Convenience accessor used by the ESP-IDF wiring.
EspBleMidi& getEspBleMidi();

} // namespace crosspad
