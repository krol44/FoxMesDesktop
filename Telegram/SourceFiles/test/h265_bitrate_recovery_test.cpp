#include "tgcalls/platform/darwin/H265BitrateRecovery.h"
#include <cassert>
#include <iostream>

namespace {
constexpr auto kHealthy = uint32_t(30000000);
constexpr auto kRecovered = uint32_t(49000000);
constexpr auto kLow = uint32_t(400000);

tgcalls::H265BitrateRecovery AfterDip() {
    auto result = tgcalls::H265BitrateRecovery();
    assert(!result.update(0, kHealthy, kLow, 25));
    assert(!result.update(1000, kLow, kLow, 40));
    assert(!result.update(2000, kLow, kLow, 51));
    return result;
}
}

int main() {
    auto stuck = AfterDip();
    for (auto time = 3000; time < 6000; time += 100) {
        assert(!stuck.update(time, kRecovered, kLow, 51));
    }
    assert(stuck.update(6000, kRecovered, kLow, 51));
    for (auto time = 6100; time < 36000; time += 100) {
        assert(!stuck.update(time, kRecovered, kLow, 51));
    }
    assert(stuck.update(36000, kRecovered, kLow, 51));
    assert(!stuck.update(37000, kRecovered, kLow, 24));
    for (auto time = 38000; time < 100000; time += 1000) {
        assert(!stuck.update(time, kRecovered, kLow, 51));
    }

    auto staticPicture = AfterDip();
    for (auto time = 3000; time < 60000; time += 100) {
        assert(!staticPicture.update(time, kRecovered, kLow, 25));
    }
    auto insufficientBandwidth = AfterDip();
    for (auto time = 3000; time < 60000; time += 100) {
        assert(!insufficientBandwidth.update(time, 1000000, kLow, 51));
    }
    auto expensivePicture = AfterDip();
    for (auto time = 3000; time < 60000; time += 100) {
        assert(!expensivePicture.update(time, kRecovered, 30000000, 51));
    }
    auto unknownQp = AfterDip();
    assert(!unknownQp.update(3000, kRecovered, kLow, -1));
    auto transient = AfterDip();
    assert(!transient.update(3000, kRecovered, kLow, 51));
    assert(!transient.update(5000, kRecovered, kLow, -1));
    assert(!transient.update(6000, kRecovered, kLow, 51));
    assert(!transient.update(8999, kRecovered, kLow, 51));
    assert(transient.update(9000, kRecovered, kLow, 51));

    auto gradual = AfterDip();
    for (auto rate = 800000; rate < 22000000; rate += 800000) {
        assert(!gradual.update(3000 + rate / 800, rate, kLow, 51));
    }
    assert(!gradual.update(40000, 25000000, kLow, 51));
    assert(gradual.update(43000, 25000000, kLow, 51));
    std::cout << "H265 bitrate recovery tests passed\n";
}
