#include "tgcalls/platform/tdesktop/DesktopHardwareEncoder.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include <cstdlib>
#include <iostream>
#define REQUIRE(value) do { if (!(value)) { std::cerr << #value << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <cstdio>

struct Counter final : webrtc::EncodedImageCallback {
    int thumbnail = 0, full = 0;
    Result OnEncodedImage(const webrtc::EncodedImage &frame, const webrtc::CodecSpecificInfo *) override {
        REQUIRE(frame.size() > 0);
        if (frame._encodedWidth == 320) ++thumbnail;
        else { REQUIRE(frame._encodedWidth == 640); ++full; }
        return Result(Result::OK);
    }
};

int main() {
    auto factory = tgcalls::MakeDesktopVideoEncoderFactory();
    for (const char *packetization : {"1", "0"}) {
        auto encoder = factory->CreateVideoEncoder(webrtc::SdpVideoFormat("H264",
            {{"profile-level-id","42e01f"},{"packetization-mode",packetization}}));
        REQUIRE(encoder);
        webrtc::VideoCodec codec{};
        codec.codecType = webrtc::kVideoCodecH264;
        codec.width = 640; codec.height = 360; codec.maxFramerate = 60;
        codec.startBitrate = 2000; codec.maxBitrate = 3000; codec.minBitrate = 100;
        codec.mode = webrtc::VideoCodecMode::kScreensharing;
        codec.active = true;
        codec.qpMax = 51;
        *codec.H264() = webrtc::VideoEncoder::GetDefaultH264Settings();
        codec.SetFrameDropEnabled(false);
        codec.numberOfSimulcastStreams = 2;
        for (int i = 0; i != 2; ++i) {
            auto &layer = codec.simulcastStream[i];
            layer.width = i ? 640 : 320; layer.height = i ? 360 : 180;
            layer.maxFramerate = i ? 60 : 5;
            layer.minBitrate = i ? 100 : 30;
            layer.targetBitrate = i ? 1900 : 100;
            layer.maxBitrate = i ? 2900 : 100;
            layer.numberOfTemporalLayers = 1; layer.qpMax = 51; layer.active = true;
        }
        auto buffer = webrtc::I420Buffer::Create(640,360);
        webrtc::I420Buffer::SetBlack(buffer.get());
        for (int pass = 0; pass != 2; ++pass) {
            Counter counter;
            encoder->RegisterEncodeCompleteCallback(&counter);
            REQUIRE(encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(
                webrtc::VideoEncoder::Capabilities(false),4,1200)) == 0);
            webrtc::VideoBitrateAllocation allocation;
            allocation.SetBitrate(0,0,100000);
            allocation.SetBitrate(1,0,1900000);
            encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(allocation,60));
            for (int i = 0; i != 120; ++i) {
                const auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
                    .set_timestamp_rtp(9000 + i*1500).set_timestamp_us(100000 + i*1000000LL/60).build();
                REQUIRE(encoder->Encode(frame,nullptr) == 0);
            }
            std::printf("packetization %s pass%d: thumbnail %d / full %d\n",packetization,pass,counter.thumbnail,counter.full);
            REQUIRE(counter.thumbnail >= 9 && counter.thumbnail <= 11);
            REQUIRE(counter.full >= 115);
            encoder->Release();
        }
    }
}
