#include "tgcalls/VideoCaptureInterfaceImpl.h"
#include "tgcalls/VideoCapturerInterface.h"
#include "tgcalls/StaticThreads.h"
#include "tgcalls/platform/tdesktop/VideoCapturerTrackSource.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "rtc_base/ref_counted_object.h"
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "Check failed: " << #condition << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <iostream>

namespace {
int creations = 0, destructions = 0, updates = 0, adaptedFps = 0;
class Capturer : public tgcalls::VideoCapturerInterface {
public:
    explicit Capturer(std::string id) : _id(std::move(id)) { ++creations; }
    ~Capturer() override { ++destructions; }
    bool updateScreenCapture(const std::string &id, std::pair<int, int> &out) override {
        if (tgcalls::ScreenSharingSourceId(id) != tgcalls::ScreenSharingSourceId(_id)) return false;
        const auto profile = tgcalls::ScreenSharingProfileForQuality(tgcalls::ScreenSharingQualityFromDeviceId(id));
        _id = id; out = {profile.width, profile.height}; ++updates;
        return true;
    }
    void setState(tgcalls::VideoState) override {}
    void setPreferredCaptureAspectRatio(float) override {}
    void setUncroppedOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) override {}
    int getRotation() override { return 0; }
private:
    std::string _id;
};
class Platform : public tgcalls::PlatformInterface {
public:
    std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(bool, bool) override { return nullptr; }
    std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override { return nullptr; }
    bool supportsEncoding(const std::string &) override { return false; }
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(rtc::Thread *, rtc::Thread *) override {
        return rtc::make_ref_counted<tgcalls::VideoCapturerTrackSource>();
    }
    void adaptVideoSource(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>, int, int, int fps) override {
        adaptedFps = fps;
    }
    std::unique_ptr<tgcalls::VideoCapturerInterface> makeVideoCapturer(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>, std::string id,
            std::function<void(tgcalls::VideoState)>, std::function<void(tgcalls::PlatformCaptureInfo)>,
            std::shared_ptr<tgcalls::PlatformContext>, std::pair<int, int> &out) override {
        out = {2560, 1440};
        return std::make_unique<Capturer>(id);
    }
};
class Threads : public tgcalls::Threads {
public:
    rtc::Thread *getNetworkThread() override { return nullptr; }
    rtc::Thread *getMediaThread() override { return nullptr; }
    rtc::Thread *getWorkerThread() override { return nullptr; }
};
}
namespace tgcalls {
std::unique_ptr<PlatformInterface> CreatePlatformInterface() { return std::make_unique<Platform>(); }
}
int main() {
    Threads threads;
    {
        tgcalls::VideoCaptureInterfaceObject capture("desktop_capturer_pipewire@foxmes-quality=4", true, nullptr, threads);
        CHECK(creations == 1 && adaptedFps == 60);
        const auto source = capture.source();
        capture.switchToDevice("desktop_capturer_pipewire@foxmes-quality=8", true);
        CHECK(creations == 1 && destructions == 0 && updates == 1);
        CHECK(capture.source() == source && adaptedFps == 30);
        CHECK(capture.screenSharingProfile().width == 1920);
        capture.switchToDevice("desktop_capturer_pipewire@foxmes-quality=3", true);
        CHECK(creations == 1 && updates == 2 && adaptedFps == 60);
        CHECK(capture.screenSharingProfile().width == 0);
        capture.switchToDevice("desktop_capturer_screen_2@foxmes-quality=4", true);
        CHECK(creations == 2 && destructions == 1 && adaptedFps == 60);
        capture.switchToDevice("camera", false);
        CHECK(creations == 3 && destructions == 2 && !capture.isScreenCapture());
    }
    CHECK(destructions == creations);
    std::cout << "Same-source profile switch preserves capturer/session; source/camera changes recreate it\n";
}
