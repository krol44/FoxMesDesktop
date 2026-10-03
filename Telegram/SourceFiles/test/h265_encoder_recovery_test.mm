// Standalone macOS regression test using synthetic NV12 frames, no screen access.
// Link the current TGRTCVideoEncoderH265 object, tgcalls and tg_owt Release archives.
// The pre-fix encoder stays at QP 51 after the rate is restored and fails the assertion.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import "tgcalls/platform/darwin/TGRTCVideoEncoderH265.h"
#import "base/RTCVideoEncoderSettings.h"
#import "base/RTCEncodedImage.h"
#import "base/RTCVideoFrame.h"
#import "components/video_frame_buffer/RTCCVPixelBuffer.h"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
void Require(bool condition) {
    if (!condition) {
        std::fprintf(stderr, "H265 recovery regression test failed\n");
        std::abort();
    }
}
}

int main(int argc, char **argv) {
 @autoreleasepool {
  const int width = argc > 1 ? atoi(argv[1]) : 2560;
  const int height = width * 9 / 16;
  auto info = [[RTCVideoCodecInfo alloc] initWithName:@"H265"];
  auto encoder = [[RTCVideoEncoderH265 alloc] initWithCodecInfo:info];
  auto settings = [[RTCVideoEncoderSettings alloc] init];
  settings.name = @"H265"; settings.width = width; settings.height = height;
  settings.startBitrate = 30000; settings.maxFramerate = 60;
  settings.mode = RTCVideoCodecModeScreensharing;
  std::atomic<int> count(0), qp(0), bytes(0);
  auto pc = &count, pq = &qp, pb = &bytes;
  [encoder setCallback:^BOOL(RTCEncodedImage *frame, id<RTCCodecSpecificInfo> csi) {
    pc->fetch_add(1); pq->fetch_add(frame.qp.intValue); pb->fetch_add(frame.buffer.length);
    return YES;
  }];
  Require([encoder startEncodeWithSettings:settings numberOfCores:8] == 0);
  auto start = std::chrono::steady_clock::now();
  for (int second = 0; second < 20; ++second) {
    const int rate = second < 5 ? 30000 : second < 10 ? 400 : 49000;
    [encoder setBitrate:rate framerate:60];
    for (int i = 0; i < 60; ++i) {
      @autoreleasepool {
        CVPixelBufferRef pixel = nullptr;
        auto attributes = @{ (id)kCVPixelBufferIOSurfacePropertiesKey : @{} };
        if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            (__bridge CFDictionaryRef)attributes, &pixel) != kCVReturnSuccess) return 1;
        CVPixelBufferLockBaseAddress(pixel, 0);
        auto y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
        auto stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
        for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
          const auto x = (col + second * 3 + i / 5);
          y[row * stride + col] = (x % 17 < 2 || row % 23 < 2) ? 235 : 16;
        }
        memset(CVPixelBufferGetBaseAddressOfPlane(pixel, 1), 128,
               CVPixelBufferGetBytesPerRowOfPlane(pixel, 1) * height / 2);
        CVPixelBufferUnlockBaseAddress(pixel, 0);
        auto buffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pixel];
        auto frame = [[RTCVideoFrame alloc] initWithBuffer:buffer rotation:RTCVideoRotation_0
                    timeStampNs:(int64_t)(second * 60 + i) * 1000000000LL / 60];
        frame.timeStamp = (second * 60 + i) * 1500;
        const auto status = [encoder encode:frame codecSpecificInfo:nil
                   frameTypes:@[@(second == 0 && i == 0 ? RTCFrameTypeVideoFrameKey : RTCFrameTypeVideoFrameDelta)]];
        CVPixelBufferRelease(pixel);
        Require(status == 0);
      }
      std::this_thread::sleep_until(start + std::chrono::microseconds((second * 60 + i + 1) * 1000000LL / 60));
    }
    auto n = count.exchange(0); auto q = qp.exchange(0); auto b = bytes.exchange(0);
    printf("sec=%d target=%d kbps encoded=%d QP=%.1f output=%d kbps\n", second, rate, n, n ? double(q)/n : -1, b*8/1000);
    if (second >= 18) {
      Require(n > 0 && double(q) / n < 40);
    }
    fflush(stdout);
  }
  [encoder releaseEncoder];
 }
}
