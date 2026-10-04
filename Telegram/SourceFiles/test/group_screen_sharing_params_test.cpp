#include "tgcalls/group/GroupScreenSharingParams.h"
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
    std::cout << "H264/VP9 group screen profile regression tests passed\n";
}
