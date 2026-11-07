#include "motion_stream.h"

#include <atomic>
#include <cmath>

#include "imu.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "Motion";

namespace {

constexpr int64_t kAliveUs = 6 * 1000 * 1000;    // the host repeats MOTION 1 every 2 s
constexpr int64_t kPeriodUs = 20 * 1000;          // 50 Hz: about the gyro's 52 Hz ODR

std::atomic<int64_t> s_aliveUntil{0};
std::atomic<uint32_t> s_events{0};               // bits not yet sent
std::atomic<uint8_t> s_d6d{0};
std::atomic<uint16_t> s_ota{0};                  // phase << 8 | percent, as last reported
uint16_t s_otaSent = 0;                          // MIDI task only

void put_ms(uint8_t *p, uint32_t ms)
{
    p[0] = (ms >> 21) & 0x7F; p[1] = (ms >> 14) & 0x7F; p[2] = (ms >> 7) & 0x7F; p[3] = ms & 0x7F;
}

void put14(uint8_t *p, float v)
{
    long i = lroundf(v);
    if (i < -8192) i = -8192;
    if (i > 8191) i = 8191;
    const unsigned u = (unsigned)(i + 8192);
    p[0] = (u >> 7) & 0x7F; p[1] = u & 0x7F;
}

} // namespace

void motion_stream_enable(bool on)
{
    const bool was = motion_stream_active();
    s_aliveUntil.store(on ? esp_timer_get_time() + kAliveUs : 0);
    if (on && !was) ESP_LOGI(TAG, "motion stream on");
}

bool motion_stream_active(void) { return esp_timer_get_time() < s_aliveUntil.load(std::memory_order_relaxed); }

void motion_stream_event(uint32_t events, uint8_t orientation_6d)
{
    if (!motion_stream_active()) return;
    s_d6d.store(orientation_6d, std::memory_order_relaxed);
    s_events.fetch_or(events, std::memory_order_relaxed);
}

void motion_stream_ota(motion_ota_phase_t phase, unsigned percent)
{
    s_ota.store((uint16_t)(((unsigned)phase << 8) | (percent > 100 ? 100 : percent)), std::memory_order_relaxed);
}

void motion_stream_pump(void (*send)(const uint8_t *frame, size_t len))
{
    static int64_t s_next = 0, s_otaNext = 0;
    if (!send || !motion_stream_active()) return;
    const int64_t now = esp_timer_get_time();
    const uint32_t ms = (uint32_t)(now / 1000);

    const uint16_t ota = s_ota.load(std::memory_order_relaxed);
    if (ota != s_otaSent && (now >= s_otaNext || (ota >> 8) != (s_otaSent >> 8))) {
        uint8_t f[12] = {0xF0, 0x7D, 0x1F, 0x0A, 0x02};
        put_ms(f + 5, ms);
        f[9] = (ota >> 8) & 0x7F; f[10] = ota & 0x7F; f[11] = 0xF7;
        send(f, sizeof f);
        s_otaSent = ota;
        s_otaNext = now + 100 * 1000;
    }

    const uint32_t ev = s_events.exchange(0, std::memory_order_relaxed);
    if (ev) {
        uint8_t f[13] = {0xF0, 0x7D, 0x1F, 0x0A, 0x01};
        put_ms(f + 5, ms);
        f[9] = ev & 0x7F; f[10] = (ev >> 7) & 0x7F; f[11] = s_d6d.load(std::memory_order_relaxed) & 0x7F;
        f[12] = 0xF7;
        send(f, sizeof f);
    }

    if (now < s_next) return;
    s_next = now + kPeriodUs;
    bsp_imu_data_t d{};
    if (bsp_imu_get_data(&d) != ESP_OK) return;
    uint8_t f[26] = {0xF0, 0x7D, 0x1F, 0x0A, 0x00};
    put_ms(f + 5, ms);
    const float v[8] = {d.accel_x * 1000.f, d.accel_y * 1000.f, d.accel_z * 1000.f,
                        d.gyro_x * 4.f, d.gyro_y * 4.f, d.gyro_z * 4.f,
                        d.fused_pitch * 40.f, d.fused_roll * 40.f};
    for (int i = 0; i < 8; ++i) put14(f + 9 + 2 * i, v[i]);
    f[25] = 0xF7;
    send(f, 26);
}
