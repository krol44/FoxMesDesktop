#include "tgcalls/ScreenSharing.h"
#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::abort();
    }
}
}

int main() {
    const std::array<int, 12> fps = {15,5,5,60,60,60,30,30,30,15,15,15};
    const std::array<int, 12> rates = {10000000,15000000,8000000,15000000,
        15000000,12000000,15000000,15000000,8000000,15000000,10000000,6000000};
    for (int i = 0; i != 12; ++i) {
        const auto profile = tgcalls::ScreenSharingProfileForQuality(i);
        Require(profile.fps == fps[i], "bitrate cap must retain selected FPS");
        Require(profile.maxBitrate == rates[i], "all profiles must respect cap and retain lower limits");
        Require(profile.startBitrate <= profile.maxBitrate, "startup allocation must fit budget");
        const auto source = i == 1 || i == 3 || i == 6 || i == 9;
        const auto fullHD = i == 5 || i == 8 || i == 11;
        Require(profile.width == (source ? 0 : fullHD ? 1920 : 2560)
            && profile.height == (source ? 0 : fullHD ? 1080 : 1440),
            "bitrate cap must retain selected dimensions");
    }
    Require(tgcalls::ClampScreenSharingBitrate(50000000) == 15000000, "encoder clamp");
    Require(tgcalls::ClampScreenSharingBitrate(400000) == 400000, "respect network dip");
    Require(tgcalls::ClampScreenSharingBitrate(0) == 0, "respect pause");
    Require(tgcalls::ScreenSharingBurstLimitBytes(15000000) == 1875000, "VT must not allow 22.5 Mbps");
    Require(tgcalls::ScreenSharingBurstLimitBytes(400000) == 75000, "retain low-rate VT allowance");

    auto recovery = tgcalls::VideoQualityRecovery();
    const auto bandwidth = [](int value) { return tgcalls::ScreenSharingRecoveryBandwidth(value, 15000000); };
    recovery.update(bandwidth(50000000), 5120 * 2880, 60, false, 60, 60);
    Require(recovery.baselineBitrate() == 15000000, "baseline must use useful video budget");
    recovery.update(bandwidth(400000), 5120 * 2880, 60, false, 60, 60);
    auto action = tgcalls::VideoRecoveryAction::None;
    for (int i = 0; i != 8; ++i) {
        action = recovery.update(bandwidth(15000000), 5120 * 2880, 60, false, 60, 60);
    }
    Require(action == tgcalls::VideoRecoveryAction::RefreshFrame,
        "recovery must not wait for the old 50 Mbps estimate");
    std::cout << "Screen bitrate cap, unchanged profiles and recovery tests passed\n";
}
