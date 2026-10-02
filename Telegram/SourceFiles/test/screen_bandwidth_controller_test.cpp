#include "tgcalls/ScreenBandwidthProbe.h"
#include "modules/congestion_controller/goog_cc/probe_bitrate_estimator.h"
#include <cassert>
#include <iostream>

using namespace tgcalls;
using namespace webrtc;

namespace {
class Delegate final : public NetworkControllerInterface {
public:
    int bitrate = 39500000;
    bool nativeProbe = false;
    NetworkControlUpdate sample() {
        NetworkControlUpdate result;
        TargetTransferRate target;
        target.target_rate = DataRate::BitsPerSec(bitrate);
        target.network_estimate.round_trip_time = TimeDelta::Millis(61);
        result.target_rate = target;
        return result;
    }
    NetworkControlUpdate OnTransportPacketsFeedback(TransportPacketsFeedback msg) override {
        auto result = sample();
        if (nativeProbe) {
            ProbeClusterConfig probe;
            probe.at_time = msg.feedback_time;
            probe.id = 7;
            result.probe_cluster_configs.push_back(probe);
            nativeProbe = false;
        }
        return result;
    }
#define FORWARD(Name, Type) NetworkControlUpdate Name(Type) override { return sample(); }
    FORWARD(OnProcessInterval, ProcessInterval)
    FORWARD(OnNetworkAvailability, NetworkAvailability)
    FORWARD(OnNetworkRouteChange, NetworkRouteChange)
    FORWARD(OnRemoteBitrateReport, RemoteBitrateReport)
    FORWARD(OnRoundTripTimeUpdate, RoundTripTimeUpdate)
    FORWARD(OnSentPacket, SentPacket)
    FORWARD(OnReceivedPacket, ReceivedPacket)
    FORWARD(OnStreamsConfig, StreamsConfig)
    FORWARD(OnTargetRateConstraints, TargetRateConstraints)
    FORWARD(OnTransportLossReport, TransportLossReport)
    FORWARD(OnNetworkStateEstimate, NetworkStateEstimate)
#undef FORWARD
};

struct Harness {
    std::shared_ptr<ScreenBandwidthProbeState> state = std::make_shared<ScreenBandwidthProbeState>();
    Delegate *delegate = nullptr;
    std::unique_ptr<ScreenBandwidthProbeController> controller;
    Harness() {
        auto implementation = std::make_unique<Delegate>();
        delegate = implementation.get();
        NetworkControllerConfig config;
        config.constraints.max_data_rate = DataRate::BitsPerSec(50000000);
        controller = std::make_unique<ScreenBandwidthProbeController>(std::move(implementation), state, config);
        state->screenLimit = 50000000;
        NetworkAvailability available;
        available.network_available = true;
        (void)controller->OnNetworkAvailability(available);
    }
    NetworkControlUpdate feedback(int64_t ms) {
        TransportPacketsFeedback report;
        report.feedback_time = Timestamp::Millis(ms);
        PacketResult packet;
        packet.sent_packet.send_time = Timestamp::Millis(ms);
        packet.receive_time = Timestamp::Millis(ms);
        report.packet_feedbacks.push_back(packet);
        return controller->OnTransportPacketsFeedback(report);
    }
    NetworkControlUpdate poll(int64_t ms) {
        (void)feedback(ms);
        ProcessInterval interval;
        interval.at_time = Timestamp::Millis(ms);
        interval.pacer_queue = DataSize::Zero();
        return controller->OnProcessInterval(interval);
    }
    void dip() {
        assert(poll(1000).probe_cluster_configs.empty());
        delegate->bitrate = 406000;
        for (int ms = 2000; ms < 7000; ms += 1000) assert(poll(ms).probe_cluster_configs.empty());
    }
};
}

int main() {
    Harness screen;
    screen.dip();
    auto update = screen.poll(7000);
    assert(update.probe_cluster_configs.size() == 1);
    const auto probe = update.probe_cluster_configs.front();
    assert(probe.target_data_rate.bps() == 812000);
    assert(probe.target_duration.ms() == 15);
    assert(probe.target_probe_count == 5);
    assert(probe.id >= 0x40000000);
    assert(update.target_rate->target_rate.bps() == 406000);
    assert(!update.pacer_config);
    ScreenSharingStats stats;
    screen.state->fillStats(stats);
    assert(stats.bandwidthProbeAttempts == 1);
    assert(stats.bandwidthProbeBitrate == 812000);

    ProbeBitrateEstimator estimator(nullptr);
    for (int i = 0; i < 5; ++i) {
        PacketResult packet;
        packet.sent_packet.send_time = Timestamp::Micros(1000000 + i * 12000);
        packet.receive_time = Timestamp::Micros(1100000 + i * 12000);
        packet.sent_packet.size = DataSize::Bytes(1200);
        packet.sent_packet.pacing_info = PacedPacketInfo(probe.id, 5, 6000);
        estimator.HandleProbeAndEstimateBitrate(packet);
    }
    const auto estimate = estimator.FetchAndResetLastEstimatedBitrate();
    assert(estimate && estimate->bps() >= 700000);

    Harness native;
    native.dip();
    native.delegate->nativeProbe = true;
    auto nativeUpdate = native.feedback(7000);
    assert(nativeUpdate.probe_cluster_configs.front().id == 7);
    assert(native.poll(7000).probe_cluster_configs.empty());
    assert(native.poll(11000).probe_cluster_configs.empty());
    assert(native.poll(12000).probe_cluster_configs.size() == 1);

    screen.state->screenLimit = 0;
    for (int ms = 8000; ms < 60000; ms += 1000) assert(screen.poll(ms).probe_cluster_configs.empty());
    std::cout << "Screen bandwidth controller and probe feedback tests passed\n";
}
