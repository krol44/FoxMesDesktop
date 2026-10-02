#include "tgcalls/ScreenBandwidthProbePolicy.h"
#include <cassert>
#include <iostream>

using namespace tgcalls;

namespace {
constexpr int kLimit = 50000000;
constexpr int kHealthy = 39500000;
constexpr int kLow = 406000;
struct Harness {
    ScreenBandwidthProbePolicy policy;
    int limit = kLimit, rtt = 61, queue = 0, feedbackAge = 100, remoteLimit = kLimit;
    double loss = 0;
    bool available = true;
    int poll(int64_t time, int bwe, bool native = false) {
        return policy.update(time, limit, bwe, rtt, loss, queue, feedbackAge, available, remoteLimit, native);
    }
    void healthy() { assert(poll(0, kHealthy) == 0); }
    void dip() {
        healthy();
        for (int second = 1; second < 6; ++second) assert(poll(second * 1000, kLow) == 0);
    }
};
}

int main() {
    Harness stalled;
    stalled.dip();
    assert(stalled.poll(6000, kLow) == 2 * kLow);
    assert(stalled.policy.status() == ScreenBandwidthProbeStatus::Probing);
    for (int t = 6100; t < 16000; t += 100) assert(stalled.poll(t, kLow) == 0);
    assert(stalled.poll(16000, kLow) == 2 * kLow);
    for (int t = 16100; t < 36000; t += 100) assert(stalled.poll(t, kLow) == 0);
    assert(stalled.poll(36000, kLow) == 2 * kLow);
    for (int t = 36100; t < 66000; t += 100) assert(stalled.poll(t, kLow) == 0);
    assert(stalled.poll(66000, kLow) == 2 * kLow);

    Harness recovering;
    recovering.dip();
    assert(recovering.poll(6000, kLow) == 2 * kLow);
    assert(recovering.poll(11000, 800000) == 1600000);
    assert(recovering.poll(16000, 1550000) == 3100000);
    assert(recovering.poll(21000, 30000000) == 0);
    assert(recovering.policy.status() == ScreenBandwidthProbeStatus::Monitoring);

    for (int reason = 0; reason < 6; ++reason) {
        Harness blocked;
        blocked.dip();
        switch (reason) {
        case 0: blocked.loss = 0.1; break;
        case 1: blocked.rtt = 1000; break;
        case 2: blocked.queue = 200; break;
        case 3: blocked.available = false; break;
        case 4: blocked.feedbackAge = 4000; break;
        case 5: blocked.remoteLimit = 450000; break;
        }
        for (int t = 6000; t < 60000; t += 1000) assert(blocked.poll(t, kLow) == 0);
    }
    Harness native;
    native.dip();
    assert(native.poll(6000, kLow, true) == 0);
    for (int t = 6100; t < 11000; t += 100) assert(native.poll(t, kLow) == 0);
    assert(native.poll(11000, kLow) == 2 * kLow);

    Harness camera;
    camera.limit = 0;
    camera.dip();
    for (int t = 6000; t < 60000; t += 1000) assert(camera.poll(t, kLow) == 0);
    assert(camera.policy.status() == ScreenBandwidthProbeStatus::Inactive);
    Harness transient;
    transient.healthy();
    assert(transient.poll(1000, kLow) == 0);
    assert(transient.poll(2000, kHealthy) == 0);
    assert(transient.policy.status() == ScreenBandwidthProbeStatus::Monitoring);

    Harness changed;
    changed.dip();
    changed.limit = 12000000;
    for (int t = 6000; t < 60000; t += 1000) assert(changed.poll(t, kLow) == 0);
    std::cout << "Screen bandwidth probing tests passed\n";
}
