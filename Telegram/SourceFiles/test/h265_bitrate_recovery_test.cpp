#include "tgcalls/platform/darwin/H265BitrateRecovery.h"
#include <cstdlib>
#define REQUIRE(value) do { if (!(value)) { std::cerr << #value << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <iostream>

namespace {
constexpr auto kHealthy = uint32_t(30000000);
constexpr auto kLow = uint32_t(400000);

tgcalls::H265BitrateRecovery AfterDip() {
    auto result = tgcalls::H265BitrateRecovery();
    REQUIRE(!result.update(0, kHealthy, kLow, 25));
    REQUIRE(!result.update(1000, kLow, kLow, 40));
    REQUIRE(!result.update(2000, kLow, kLow, 51));
    return result;
}
}

int main() {
    // Recorded failure: target stays at 9.8 Mbps instead of returning to the
    // old 30 Mbps peak; QP 51 and 228 kbps output must still trigger recovery.
    auto stuck = AfterDip();
    for (auto time = 3000; time < 6000; time += 100) {
        REQUIRE(!stuck.update(time, 9800000, 228000, 51));
    }
    REQUIRE(stuck.update(6000, 9800000, 228000, 51));
    for (auto time = 6100; time < 36000; time += 100) {
        REQUIRE(!stuck.update(time, 9800000, 228000, 51));
    }
    REQUIRE(stuck.update(36000, 9800000, 228000, 51));
    REQUIRE(!stuck.update(37000, 9800000, 228000, 24));
    REQUIRE(!stuck.update(38000, 9800000, 228000, 51));
    REQUIRE(!stuck.update(65999, 9800000, 228000, 51));
    REQUIRE(stuck.update(66000, 9800000, 228000, 51));

    auto startup = tgcalls::H265BitrateRecovery();
    REQUIRE(!startup.update(0, 9800000, 228000, 51));
    REQUIRE(!startup.update(2999, 9800000, 228000, 51));
    REQUIRE(startup.update(3000, 9800000, 228000, 51));

    auto staticPicture = AfterDip();
    auto insufficientBandwidth = AfterDip();
    auto expensivePicture = AfterDip();
    auto paused = AfterDip();
    auto noOutput = AfterDip();
    for (auto time = 3000; time < 60000; time += 100) {
        REQUIRE(!staticPicture.update(time, 9800000, 228000, 25));
        REQUIRE(!insufficientBandwidth.update(time, 1000000, 228000, 51));
        REQUIRE(!expensivePicture.update(time, 9800000, 8000000, 51));
        REQUIRE(!paused.update(time, 0, 228000, 51));
        REQUIRE(!noOutput.update(time, 9800000, 0, 51));
    }
    auto transient = AfterDip();
    REQUIRE(!transient.update(3000, 9800000, 228000, 51));
    REQUIRE(!transient.update(5000, 9800000, 228000, -1));
    REQUIRE(!transient.update(6000, 9800000, 228000, 51));
    REQUIRE(!transient.update(8999, 9800000, 228000, 51));
    REQUIRE(transient.update(9000, 9800000, 228000, 51));

    auto changing = AfterDip();
    REQUIRE(!changing.update(0, 4000000, 228000, 51));
    REQUIRE(!changing.update(2000, 8000000, 228000, 51));
    REQUIRE(!changing.update(4000, 15000000, 228000, 51));
    REQUIRE(!changing.update(6000, 9800000, 228000, 51));
    REQUIRE(!changing.update(8999, 9800000, 228000, 51));
    REQUIRE(changing.update(9000, 9800000, 228000, 51));
    std::cout << "H265 current-allocation recovery and cooldown tests passed\n";
}
