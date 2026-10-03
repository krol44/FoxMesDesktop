#include "tgcalls/ScreenSharingPerformance.h"
#include "tgcalls/platform/darwin/ScreenEncodingSpeed.h"
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::abort();
    }
}

tgcalls::ScreenSharingStats Healthy(int fps = 60) {
    tgcalls::ScreenSharingStats stats;
    stats.fps = fps;
    stats.encoderInputFps = fps;
    stats.encodeUsagePercent = 60;
    stats.powerEfficientKnown = stats.powerEfficient = true;
    stats.availableBitrate = 20000000;
    stats.targetBitrate = 19000000;
    stats.codec = "H265";
    stats.averageQp = 24;
    return stats;
}

bool Sample(tgcalls::ScreenSharingPerformance &policy, int64_t &now,
        const tgcalls::ScreenSharingStats &stats, int count) {
    auto changed = false;
    for (auto i = 0; i < count; ++i) {
        changed |= policy.update(now, stats);
        now += 1000;
    }
    return changed;
}
}

int main() {
    auto policy = tgcalls::ScreenSharingPerformance();
    auto now = int64_t(0);
    auto stats = Healthy(24);
    stats.cpuLimited = true;
    stats.encodeUsagePercent = 55;
    policy.reset(60, now);
    Require(!Sample(policy, now, stats, 12), "must ignore initial estimator and warm-up");
    Require(Sample(policy, now, stats, 8) && policy.fps() == 60,
        "24 FPS with spare capacity must probe the selected 60 FPS");
    Require(Sample(policy, now, stats, 20) && policy.fps() == 50,
        "failed probe must reduce one step instead of endlessly resetting");
    Require(!Sample(policy, now, stats, 60), "failed probe must back off");

    policy.reset(30, now);
    Require(policy.fps() == 30, "manual mode change must clear the adaptive ceiling");
    stats = Healthy(30);
    Require(!Sample(policy, now, stats, 100), "must never exceed a manual 30 FPS ceiling");

    policy.reset(60, now);
    stats = Healthy(60);
    stats.encodeUsagePercent = 210;
    Require(Sample(policy, now, stats, 20) && policy.fps() == 50,
        "persistent real overload must lower FPS even before WebRTC restricts it");
    stats = Healthy(50);
    Require(Sample(policy, now, stats, 60) && policy.fps() == 60,
        "stable spare capacity must restore the next FPS tier");

    policy.reset(60, now);
    stats = Healthy(24);
    stats.cpuLimited = true;
    stats.limitation = "bandwidth";
    Require(!Sample(policy, now, stats, 100), "network restrictions must not trigger CPU resets");
    stats.limitation = "CPU";
    stats.availableBitrate = 406000;
    stats.targetBitrate = 358000;
    Require(!Sample(policy, now, stats, 100), "low BWE must not look like CPU recovery");
    stats = Healthy(24);
    stats.cpuLimited = true;
    stats.averageQp = 51;
    Require(Sample(policy, now, stats, 8) && policy.fps() == 50,
        "bad image quality must block upward probes and allow reducing CPU load");

    policy.reset(5, now);
    stats = Healthy(5);
    stats.cpuLimited = true;
    stats.encodeUsagePercent = 210;
    Require(!Sample(policy, now, stats, 100), "must not reset forever at the FPS floor");

    auto speed = tgcalls::ScreenEncodingSpeed();
    Require(!speed.update(0, 60, 24, 20000000, 24, false), "speed warm-up");
    const auto enable = speed.update(2000, 60, 24, 20000000, 24, false);
    Require(enable && *enable, "must try faster encoding before lowering FPS");
    Require(!speed.update(3000, 60, 60, 20000000, 24, true), "quality needs a stable window");
    const auto quality = speed.update(33000, 60, 60, 20000000, 24, true);
    Require(quality && !*quality, "must restore quality when FPS is stable");
    Require(!speed.update(34000, 60, 24, 400000, 24, false), "low bitrate must not enable speed");
    auto degraded = tgcalls::ScreenEncodingSpeed();
    Require(!degraded.update(0, 60, 24, 20000000, 45, true), "QP guard needs stability");
    const auto guard = degraded.update(5000, 60, 24, 20000000, 45, true);
    Require(guard && !*guard, "must withdraw speed priority when QP becomes excessive");
    std::cout << "Screen performance and encoding speed policy tests passed\n";
}
