#include "tgcalls/group/GroupScreenSharingParams.h"
#include "modules/video_coding/include/video_codec_initializer.h"
#include "modules/video_coding/svc/svc_rate_allocator.h"
#include "modules/video_coding/svc/scalability_mode_util.h"
#include "api/video_codecs/video_encoder.h"
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool value) {
    if (!value) std::abort();
}
}

int main() {
    webrtc::RtpParameters params;
    params.encodings.resize(2);
    params.encodings[0].ssrc = 123;
    params.encodings[1].ssrc = 125;
    params.encodings[0].scale_resolution_down_by = 2.0;
    params.encodings[0].max_bitrate_bps = 100000;
    params.encodings[1].max_bitrate_bps = 19900000;
    const auto profile = tgcalls::ScreenSharingProfileForQuality(4);
    tgcalls::ConfigureVp9ScreenSharing(params, profile, 720);
    Require(params.encodings[0].max_bitrate_bps == 15000000);
    Require(params.encodings[0].scale_resolution_down_by == 1.0);
    Require(params.encodings[0].active && !params.encodings[1].active);
    Require(params.encodings[0].ssrc == 123 && params.encodings[1].ssrc == 125);
    tgcalls::ConfigureVp9ScreenSharing(params, tgcalls::ScreenSharingProfileForQuality(11), 720);
    Require(params.encodings[0].max_bitrate_bps == 6000000);
    Require(params.encodings[0].scale_resolution_down_by == 1.0);
    tgcalls::ConfigureVp9ScreenSharing(params, profile, 0);
    Require(!params.encodings[0].active && !params.encodings[1].active);
    tgcalls::ConfigureVp9ScreenSharing(params, profile, 360);
    Require(params.encodings[0].active);
    params.encodings.resize(1);
    tgcalls::ConfigureVp9ScreenSharing(params, profile, 720);
    Require(params.encodings[0].max_bitrate_bps == 15000000);
    params.encodings.resize(2);
    params.encodings[0].ssrc = 123;
    params.encodings[1].ssrc = 125;
    tgcalls::ConfigureH264ScreenSharing(params, profile, 720, 60);
    Require(params.encodings[0].requested_resolution->height == 360);
    Require(params.encodings[0].max_framerate == 5);
    Require(params.encodings[1].requested_resolution->height == 1440);
    Require(params.encodings[1].max_framerate == 60);
    Require(params.encodings[0].active && params.encodings[1].active);
    Require(!params.encodings[0].scale_resolution_down_by && !params.encodings[1].scale_resolution_down_by);
    Require(params.encodings[0].ssrc == 123 && params.encodings[1].ssrc == 125);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 360, 30);
    Require(params.encodings[0].active && !params.encodings[1].active);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 180, 30);
    Require(params.encodings[0].active && !params.encodings[1].active);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 0, 60);
    Require(!params.encodings[0].active && !params.encodings[1].active);
    tgcalls::ConfigureH264ScreenSharing(params, tgcalls::ScreenSharingProfileForQuality(3), 720, 60);
    Require(!params.encodings[1].requested_resolution);
    params.encodings.resize(1);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 360, 30);
    Require(params.encodings[0].active && params.encodings[0].max_framerate == 30);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 0, 30);
    Require(!params.encodings[0].active);
    params.encodings.resize(3);
    tgcalls::ConfigureH264ScreenSharing(params, profile, 720, 60);
    Require(params.encodings[0].requested_resolution->height == 180);
    Require(params.encodings[1].requested_resolution->height == 360);
    Require(*params.encodings[0].max_bitrate_bps + *params.encodings[1].max_bitrate_bps
        + *params.encodings[2].max_bitrate_bps == profile.maxBitrate);
    // Check the effective codec and allocator, not merely RTP parameter values.
    // The legacy two-spatial-layer setup used to cap this at 5 FPS / 250 kbps.
    params.encodings.resize(1);
    tgcalls::ConfigureVp9ScreenSharing(params, profile, 720);
    Require(params.encodings[0].scalability_mode == "L1T1");
    webrtc::VideoEncoderConfig config;
    config.codec_type = webrtc::kVideoCodecVP9;
    config.content_type = webrtc::VideoEncoderConfig::ContentType::kScreen;
    config.number_of_streams = 1;
    config.max_bitrate_bps = *params.encodings[0].max_bitrate_bps;
    config.simulcast_layers.resize(1);
    config.simulcast_layers[0].active = true;
    auto vp9 = webrtc::VideoEncoder::GetDefaultVp9Settings();
    vp9.numberOfSpatialLayers = 1;
    vp9.numberOfTemporalLayers = 1;
    vp9.flexibleMode = true;
    config.encoder_specific_settings = new rtc::RefCountedObject<webrtc::VideoEncoderConfig::Vp9EncoderSpecificSettings>(vp9);
    webrtc::VideoStream stream;
    stream.width = 2560;
    stream.height = 1440;
    stream.max_framerate = 60;
    stream.min_bitrate_bps = *params.encodings[0].min_bitrate_bps;
    stream.target_bitrate_bps = stream.max_bitrate_bps = config.max_bitrate_bps;
    stream.max_qp = 56;
    stream.active = true;
    stream.scalability_mode = webrtc::ScalabilityModeFromString(*params.encodings[0].scalability_mode);
    webrtc::VideoCodec codec;
    Require(webrtc::VideoCodecInitializer::SetupCodec(config, {stream}, &codec));
    Require(codec.VP9()->numberOfSpatialLayers == 1);
    Require(codec.spatialLayers[0].maxFramerate == 60);
    Require(codec.spatialLayers[0].maxBitrate == profile.maxBitrate / 1000);
    webrtc::SvcRateAllocator allocator(codec);
    const auto allocation = allocator.Allocate(webrtc::VideoBitrateAllocationParameters(2800000u, 60u));
    Require(allocation.get_sum_bps() == 2800000);
    std::cout << "H264/VP9 group screen profile regression tests passed\n";
}
