#include "tgcalls/platform/tdesktop/DesktopVideoFrameAdapter.h"
#include "tgcalls/desktop_capturer/DesktopCaptureSchedule.h"
#include "api/video/i420_buffer.h"
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) { std::cerr << "Check failed: " << #condition << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <iostream>

namespace {
class Sink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    void OnFrame(const webrtc::VideoFrame &frame) override {
        ++frames;
        CHECK(frame.width() == 320 && frame.height() == 180);
    }
    void OnDiscardedFrame() override { ++discarded; }
    int frames = 0, discarded = 0;
};
}

int main() {
    tgcalls::DesktopVideoFrameAdapter adapter;
    Sink sink;
    int captureFps = 0;
    adapter.setCaptureFpsUpdated([&](int fps) { captureFps = fps; });
    adapter.setSelectedFps(60);
    rtc::VideoSinkWants wants;
    wants.is_active = true;
    wants.max_framerate_fps = 60;
    wants.max_pixel_count = 256; // An explicit profile retains its dimensions.
    adapter.AddOrUpdateSink(&sink, wants);
    CHECK(captureFps == 60);
    const auto buffer = webrtc::I420Buffer::Create(320, 180);
    int64_t timestamp = 1000000;
    auto feed = [&] {
        sink.frames = sink.discarded = 0;
        for (int i = 0; i != 120; ++i) {
            const auto frame = webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_timestamp_us(timestamp).build();
            timestamp += 16667;
            adapter.OnFrame(frame);
        }
        return sink.frames;
    };
    CHECK(feed() == 120);
    wants.max_framerate_fps = 30;
    adapter.AddOrUpdateSink(&sink, wants);
    CHECK(captureFps == 30);
    const auto limited30 = feed();
    CHECK(limited30 >= 60 && limited30 <= 61 && sink.discarded == 120 - limited30);
    wants.max_framerate_fps = 60;
    adapter.AddOrUpdateSink(&sink, wants);
    CHECK(captureFps == 60 && feed() == 120);
    adapter.setSelectedFps(15);
    CHECK(captureFps == 15);
    const auto limited15 = feed();
    CHECK(limited15 >= 30 && limited15 <= 31);
    adapter.setSelectedFps(60);
    CHECK(captureFps == 60 && feed() == 120);
    wants.max_framerate_fps = 0;
    adapter.AddOrUpdateSink(&sink, wants);
    CHECK(captureFps == 0 && feed() == 0);
    wants.max_framerate_fps = 60;
    adapter.AddOrUpdateSink(&sink, wants);
    CHECK(captureFps == 60 && feed() == 120);
    adapter.RemoveSink(&sink);
    adapter.setCaptureFpsUpdated(nullptr);

    tgcalls::DesktopCaptureSchedule schedule(60);
    schedule.reset(1000000);
    CHECK(schedule.next(1000000) == 1016667);
    schedule.setFps(30, 1018000);
    CHECK(schedule.next(1018000) == 1051333);
    CHECK(schedule.next(1052000) == 1084667);
    schedule.setFps(60, 1085000);
    CHECK(schedule.next(1085000) == 1101667);
    CHECK(schedule.next(1102500) == 1118333);
    // No old cadence backlog after a long stall or lower-to-higher switch.
    schedule.setFps(15, 9000000);
    CHECK(schedule.next(9000000) == 9066667);
    const auto overdue = schedule.next(10000000);
    CHECK(overdue >= 10000000 && overdue <= 10066667);
    std::cout << "Desktop FPS restriction, restoration, selected ceiling and cadence tests passed\n";
}
