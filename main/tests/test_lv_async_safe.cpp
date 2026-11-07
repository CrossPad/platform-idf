// SPDX-License-Identifier: GPL-3.0-or-later
// main/lv_async_safe.cpp over host stand-ins (tests/fakes): a caller of
// lv_async_call_locked() never waits for the display lock.
#include <catch2/catch_test_macros.hpp>

#include "esp_lvgl_port.h"
#include "lv_async_safe.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::mutex s_m;
std::condition_variable s_cv;
std::vector<int> s_ran;

void record(void* user) {
    std::lock_guard<std::mutex> lock(s_m);
    s_ran.push_back(static_cast<int>(reinterpret_cast<intptr_t>(user)));
    s_cv.notify_all();
}

void post(int n) { lv_async_call_locked(record, reinterpret_cast<void*>(static_cast<intptr_t>(n))); }

void waitRan(size_t count) {
    std::unique_lock<std::mutex> lock(s_m);
    s_cv.wait(lock, [count] { return s_ran.size() >= count; });
}

} // namespace

TEST_CASE("lv_async_call_locked: never waits for the display lock, keeps the order", "[lv_async]") {
    post(0);
    post(1);
    lv_async_safe_start();
    waitRan(2);

    // The LVGL thread holds the display lock and waits for the caller to
    // return -- PadManager deactivation waiting for a pad callback that posts
    // GUI work. The forwarder takes one call and blocks on the lock; the
    // queue behind it fills, and the calls past it must not wait.
    constexpr int kPosts = 26;
    REQUIRE(lvgl_port_lock(0));

    std::mutex doneM;
    std::condition_variable doneCv;
    bool done = false;
    std::thread caller([&] {
        for (int n = 2; n < 2 + kPosts; ++n) post(n);
        std::lock_guard<std::mutex> lock(doneM);
        done = true;
        doneCv.notify_all();
    });

    bool returned;
    {
        std::unique_lock<std::mutex> lock(doneM);
        returned = doneCv.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
    }
    CHECK(returned);

    lvgl_port_unlock();
    caller.join();
    waitRan(2 + kPosts);

    std::vector<int> expected;
    for (int n = 0; n < 2 + kPosts; ++n) expected.push_back(n);
    std::lock_guard<std::mutex> lock(s_m);
    CHECK(s_ran == expected);
}
