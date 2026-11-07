/**
 * @file host_info.cpp
 * @brief HOST_INFO / FW_LATEST companion verbs — see host_info.h.
 */

#include "host_info.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "fw_updater.h"
#include "crosspad/fw/FwCatalog.hpp"
#include "crosspad/fw/Semver.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

extern crosspad::CrosspadStatus status;

static_assert(sizeof(crosspad::CrosspadStatus::fwLatest) >= FWU_VERSION_LEN,
              "a version the updater persists must fit the status field");

namespace {

const char *TAG = "HostInfo";
const char *kNvsNamespace = "host_info";
const char *kNvsLatestKey = "fw_latest";

/* The host sends whole seconds every 2 s; stepping the clock on every report
 * would make it jitter by the transport delay. */
constexpr long long kClockSlackS = 2;
constexpr int kMinutesPerHour = 60;
constexpr unsigned kMaxCpuPct = 100;

int s_tzMinutes = 0;

void apply_latest(const char *latest)
{
    strlcpy(status.fwLatest, latest, sizeof(status.fwLatest));
    const esp_app_desc_t *app = esp_app_get_description();
    crosspad::Semver want, have;
    status.fwUpdateAvailable = app && crosspad::Semver::parse(latest, want) &&
                               crosspad::Semver::parse(app->version, have) && have < want;
}

/* FW_LATEST runs on the CDC loop or the companion task, the catalog check on
 * the updater's job task: one decision at a time on status.fwLatest. */
SemaphoreHandle_t latest_lock()
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

void persist_latest(const char *latest)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, kNvsLatestKey, latest) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/* tz is minutes east of UTC; POSIX TZ counts the other way ("UTC-02:00" is
 * two hours ahead). */
void set_clock(long long unixSeconds, int tzMinutes)
{
    struct timeval now;
    gettimeofday(&now, nullptr);
    if (llabs(static_cast<long long>(now.tv_sec) - unixSeconds) > kClockSlackS) {
        struct timeval tv = {static_cast<time_t>(unixSeconds), 0};
        settimeofday(&tv, nullptr);
    }
    if (tzMinutes != s_tzMinutes) {
        const int a = abs(tzMinutes);
        char tz[16];
        snprintf(tz, sizeof(tz), "UTC%c%02d:%02d", tzMinutes >= 0 ? '-' : '+',
                 a / kMinutesPerHour, a % kMinutesPerHour);
        setenv("TZ", tz, 1);
        tzset();
        s_tzMinutes = tzMinutes;
    }
}

/* Verb match with its argument separator, so HOST_INFOX is not HOST_INFO. */
const char *verb_args(const char *cmd, const char *verb)
{
    const size_t n = strlen(verb);
    if (strncmp(cmd, verb, n) != 0) return nullptr;
    if (cmd[n] == '\0') return cmd + n;
    if (cmd[n] == ' ') return cmd + n + 1;
    return nullptr;
}

/* noinline: the text buffer is on the stack only while a report is written,
 * never in host_info_handle()'s frame, which every CDC verb passes through. */
constexpr size_t kReplyLen = 112;

__attribute__((noinline)) void report_host(void (*reply)(const char *))
{
    char text[kReplyLen];
    const int64_t now = crosspad::getClock().getTimeUs();
    const long long ageMs = status.hostSeenUs ? (now - status.hostSeenUs) / 1000 : -1;
    snprintf(text, sizeof(text), "HOST: alive=%d age_ms=%lld cpu=%u ram=%lu/%lu time=%lld daws=%u/%u\r\n",
             status.hostAlive(now) ? 1 : 0, ageMs, static_cast<unsigned>(status.hostCpuPct),
             static_cast<unsigned long>(status.hostRamUsedMb),
             static_cast<unsigned long>(status.hostRamTotalMb),
             static_cast<long long>(time(nullptr)),
             static_cast<unsigned>(status.hostDawsInstalled),
             static_cast<unsigned>(status.hostDawsRunning));
    reply(text);
}

__attribute__((noinline)) void report_latest(void (*reply)(const char *))
{
    char text[kReplyLen];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(text, sizeof(text), "FW_LATEST: latest=%s running=%s update=%d\r\n",
             status.fwLatest[0] ? status.fwLatest : "-", app ? app->version : "-",
             status.fwUpdateAvailable ? 1 : 0);
    reply(text);
}

__attribute__((noinline)) bool take_host_info(const char *args)
{
    unsigned cpu = 0;
    unsigned long used = 0, total = 0;
    long long unixSeconds = 0;
    int tz = 0;
    int consumed = 0;
    if (sscanf(args, "cpu=%u ram=%lu/%lu time=%lld tz=%d%n",
               &cpu, &used, &total, &unixSeconds, &tz, &consumed) != 5) {
        return false;
    }
    // Optional: a host that cannot launch a DAW (or predates `daws=`) is 0/0.
    unsigned installed = 0, running = 0;
    if (sscanf(args + consumed, " daws=%u/%u", &installed, &running) != 2) {
        installed = running = 0;
    }
    status.hostDawsInstalled = static_cast<uint8_t>(installed);
    status.hostDawsRunning = static_cast<uint8_t>(running);
    status.hostCpuPct = static_cast<uint8_t>(cpu > kMaxCpuPct ? kMaxCpuPct : cpu);
    status.hostRamUsedMb = used;
    status.hostRamTotalMb = total;
    status.hostSeenUs = crosspad::getClock().getTimeUs();
    set_clock(unixSeconds, tz);
    return true;
}

} // namespace

bool host_info_offer_latest(const char *version)
{
    if (!crosspad::fwVersionValid(version)) {
        ESP_LOGW(TAG, "firmware version refused: not a version, not a safe name or longer than %u",
                 static_cast<unsigned>(sizeof(status.fwLatest) - 1));
        return false;
    }
    /* The board's own catalog check knows its kit and channel; the hub's
     * kit-agnostic FW_LATEST does not. Once a check has succeeded its result
     * stands, and a hub only informs a board that has never checked. */
    if (fw_updater_checked()) return false;
    xSemaphoreTake(latest_lock(), portMAX_DELAY);
    const bool newer = !status.fwLatest[0] || crosspad::compareVersionText(version, status.fwLatest) > 0;
    if (newer) {
        apply_latest(version);
        persist_latest(version);
    }
    xSemaphoreGive(latest_lock());
    if (newer) {
        ESP_LOGI(TAG, "latest firmware %s, update %s", version,
                 status.fwUpdateAvailable ? "available" : "not needed");
    }
    return newer;
}

void host_info_set_latest(const char *version)
{
    if (strlen(version) >= sizeof(status.fwLatest)) {
        ESP_LOGW(TAG, "catalog version \"%s\" refused: longer than %u", version,
                 static_cast<unsigned>(sizeof(status.fwLatest) - 1));
        return;
    }
    xSemaphoreTake(latest_lock(), portMAX_DELAY);
    apply_latest(version);
    persist_latest(version);
    xSemaphoreGive(latest_lock());
    ESP_LOGI(TAG, "catalog check: latest %s, update %s", version[0] ? version : "-",
             status.fwUpdateAvailable ? "available" : "not needed");
}

void host_info_init(void)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    char latest[sizeof(status.fwLatest)];
    size_t len = sizeof(latest);
    if (nvs_get_str(h, kNvsLatestKey, latest, &len) == ESP_OK) {
        apply_latest(latest);
        ESP_LOGI(TAG, "latest firmware %s, update %s", latest,
                 status.fwUpdateAvailable ? "available" : "not needed");
    }
    nvs_close(h);
}

bool host_info_handle(const char *cmd, void (*reply)(const char *text))
{
    if (const char *args = verb_args(cmd, "HOST_INFO")) {
        if (*args && !take_host_info(args)) {
            reply("ERR HOST_INFO cpu=PCT ram=USED/TOTAL time=UNIX tz=MIN [daws=INST/RUN]\r\n");
            return true;
        }
        report_host(reply);
        return true;
    }
    if (const char *args = verb_args(cmd, "FW_LATEST")) {
        if (*args) {
            if (!crosspad::fwVersionValid(args)) {
                reply("ERR FW_LATEST x.y.z\r\n");
                return true;
            }
            host_info_offer_latest(args);
        }
        report_latest(reply);
        return true;
    }
    return false;
}
