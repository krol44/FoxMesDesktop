#include "tgcalls/ScreenSharing.h"
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; std::abort(); }
}
tgcalls::ScreenSharingStats Stalled(const char *codec, const char *encoder) {
    tgcalls::ScreenSharingStats stats;
    stats.targetBitrate = 9800000;
    stats.bitrate = 228000;
    stats.averageQp = 50.9;
    stats.codec = codec;
    stats.encoder = encoder;
    stats.rttMs = 10;
    return stats;
}
tgcalls::VideoRecoveryAction Sample(tgcalls::VideoQualityRecovery &recovery,
        const tgcalls::ScreenSharingStats &stats, int bandwidth = 10000000, bool cpu = false) {
    return recovery.update(bandwidth, 2560 * 1440, 60, cpu, 60, 56.7, &stats);
}
}

int main() {
    using Action = tgcalls::VideoRecoveryAction;
    for (const auto *encoder : {"FFmpeg/h264_nvenc", "FFmpeg/h264_mf", "FFmpeg/h264_vaapi", "OpenH264"}) {
        auto stats = Stalled("H264", encoder);
        auto recovery = tgcalls::VideoQualityRecovery();
        recovery.update(19200000, 2560 * 1440, 60, false, 60, 60);
        recovery.update(400000, 2560 * 1440, 60, false, 60, 60);
        for (int i = 0; i < 3; ++i) Require(Sample(recovery, stats) == Action::None, "require stable encoder evidence");
        Require(Sample(recovery, stats) == Action::ResetAdaptation, "reset at 10 Mbps without waiting for 19.2 Mbps");
        Require(recovery.baselineBitrate() == 10000000, "use current useful bandwidth");
        for (int i = 0; i < 29; ++i) Require(Sample(recovery, stats) == Action::None, "do not reset every stats tick");
        Require(Sample(recovery, stats) == Action::ResetAdaptation, "retry failed recovery after cooldown");
    }
    auto native = tgcalls::VideoQualityRecovery();
    auto stats = Stalled("H265", "VideoToolbox");
    for (int i = 0; i < 7; ++i) Require(Sample(native, stats) == Action::None, "allow local VT watchdog to act first");
    Require(Sample(native, stats) == Action::ResetAdaptation, "escalate if local VT recovery did not help");

    auto recovered = tgcalls::VideoQualityRecovery();
    recovered.update(19200000, 2560 * 1440, 60, false, 60, 60);
    recovered.update(400000, 2560 * 1440, 60, false, 60, 60);
    stats.averageQp = 24;
    for (int i = 0; i < 3; ++i) Require(Sample(recovered, stats) == Action::None, "require stable current allocation");
    Require(Sample(recovered, stats) == Action::RefreshFrame, "healthy low-rate image can recover below old peak");

    for (int scenario = 0; scenario != 8; ++scenario) {
        auto guarded = tgcalls::VideoQualityRecovery();
        auto value = Stalled("H264", "FFmpeg/h264_nvenc");
        if (scenario == 0) value.averageQp = 24; // Quiet, well-encoded image.
        if (scenario == 1) value.averageQp = -1;
        if (scenario == 2) value.bitrate = 8000000; // Complex content using its allocation.
        if (scenario == 3) value.pacerDelayMs = 200;
        if (scenario == 4) value.lossPercent = 5;
        if (scenario == 5) value.rttMs = 600;
        if (scenario == 6) value.targetBitrate = 500000;
        for (int i = 0; i < 100; ++i) {
            Require(Sample(guarded, value, 10000000, scenario == 7) == Action::None, "must not restart normal or overloaded streams");
        }
    }
    auto poorNetwork = tgcalls::VideoQualityRecovery();
    stats = Stalled("H264", "OpenH264");
    for (int i = 0; i < 100; ++i) Require(Sample(poorNetwork, stats, 1000000) == Action::None, "allocation must be supported by current BWE");
    auto windowsUnknownRtt = tgcalls::VideoQualityRecovery();
    stats.rttMs = -1;
    for (int i = 0; i < 3; ++i) Require(Sample(windowsUnknownRtt, stats) == Action::None, "unknown RTT warmup");
    Require(Sample(windowsUnknownRtt, stats) == Action::ResetAdaptation, "missing RTT must not disable recovery on Windows/Linux");
    auto vp9 = tgcalls::VideoQualityRecovery();
    stats = Stalled("VP9", "libvpx"); stats.averageQp = 208;
    for (int i = 0; i < 3; ++i) Require(Sample(vp9, stats) == Action::None, "VP9 uses its own QP scale");
    Require(Sample(vp9, stats) == Action::ResetAdaptation, "VP9 coarse undershoot recovery");
    std::cout << "Cross-platform encoder recovery, current allocation and network guards passed\n";
}
