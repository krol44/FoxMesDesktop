// Standalone macOS tests. Synthetic IOSurface frames only; never capture a screen.
// Link MacScreenCapture and current H264/H265 encoder objects with Release archives.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import "components/video_frame_buffer/RTCCVPixelBuffer.h"
#import "base/RTCVideoEncoderSettings.h"
#import "base/RTCEncodedImage.h"
#import "base/RTCVideoFrame.h"
#import "tgcalls/platform/darwin/TGRTCVideoEncoderH264.h"
#import "tgcalls/platform/darwin/TGRTCVideoEncoderH265.h"
#include "tgcalls/platform/darwin/MacScreenCapture.h"
#include "sdk/objc/native/src/objc_frame_buffer.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void Require(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "%s\n", message); std::abort(); }
}
CMSampleBufferRef Sample(CVPixelBufferRef pixel) {
    CMVideoFormatDescriptionRef format = nullptr;
    Require(CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixel, &format) == noErr, "format creation");
    const CMSampleTimingInfo timing = {CMTimeMake(1, 60), CMTimeMake(1, 60), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    Require(CMSampleBufferCreateReadyWithImageBuffer(nullptr, pixel, format, &timing, &sample) == noErr, "sample creation");
    CFRelease(format);
    return sample;
}
CVPixelBufferRef Pixel(int width, int height, int index) {
    CVPixelBufferRef pixel = nullptr;
    auto attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    Require(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
        (__bridge CFDictionaryRef)attributes, &pixel) == kCVReturnSuccess, "pixel allocation");
    CVPixelBufferLockBaseAddress(pixel, 0);
    auto y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
    const auto stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col)
        y[row * stride + col] = ((col + index * 3) % 31 < 3 || row % 29 < 3) ? 235 : 16;
    memset(CVPixelBufferGetBaseAddressOfPlane(pixel, 1), 128,
           CVPixelBufferGetBytesPerRowOfPlane(pixel, 1) * height / 2);
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    return pixel;
}
void BufferAndSizes() {
    auto pixel = Pixel(1280, 720, 0);
    const auto surface = CVPixelBufferGetIOSurface(pixel);
    const auto baseline = IOSurfaceGetUseCount(surface);
    @autoreleasepool {
        auto sample = Sample(pixel);
        auto native = tgcalls::MacScreenFrame(sample);
        CFRelease(sample);
        Require(native && native->type() == webrtc::VideoFrameBuffer::Type::kNative, "native type lost");
        Require(IOSurfaceGetUseCount(surface) == baseline + 1, "surface not leased");
        auto objc = (RTCCVPixelBuffer *)webrtc::ToObjCVideoFrameBuffer(native);
        Require(objc.pixelBuffer == pixel, "unexpected source copy");
        auto cropped = native->CropAndScale(100, 40, 1000, 600, 500, 300);
        Require(cropped->type() == webrtc::VideoFrameBuffer::Type::kNative, "crop converted to I420");
        native = nullptr;
        objc = nil;
        Require(IOSurfaceGetUseCount(surface) == baseline + 1, "crop lost surface ownership");
        auto croppedObjc = (RTCCVPixelBuffer *)webrtc::ToObjCVideoFrameBuffer(cropped);
        Require(croppedObjc.pixelBuffer == pixel && croppedObjc.cropX == 100 && croppedObjc.cropY == 40, "crop geometry");
        auto second = cropped->CropAndScale(50, 30, 400, 240, 200, 120);
        auto secondObjc = (RTCCVPixelBuffer *)webrtc::ToObjCVideoFrameBuffer(second);
        Require(secondObjc.cropX == 200 && secondObjc.cropY == 100 && secondObjc.cropWidth == 800, "nested crop geometry");
        auto i420 = second->ToI420();
        Require(i420 && i420->width() == 200 && i420->height() == 120, "software codec fallback");
    }
    Require(IOSurfaceGetUseCount(surface) == baseline, "surface lease leaked");
    CVPixelBufferRelease(pixel);
    @autoreleasepool {
        auto padded = Pixel(1280, 720, 0);
        auto sample = Sample(padded);
        auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
        auto metadata = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        auto rect = CGRectCreateDictionaryRepresentation(CGRectMake(100, 40, 1000, 600));
        // Use the framework's keys, which are not required to equal their symbol names.
        if (@available(macOS 12.3, *)) {
            CFDictionarySetValue(metadata, (__bridge CFStringRef)SCStreamFrameInfoContentRect, rect);
            CFDictionarySetValue(metadata, (__bridge CFStringRef)SCStreamFrameInfoScaleFactor, (__bridge CFNumberRef)@1);
        }
        CFRelease(rect);
        auto frame = tgcalls::MacScreenFrame(sample);
        Require(frame->width() == 1000 && frame->height() == 600, "visible content size");
        auto objc = (RTCCVPixelBuffer *)webrtc::ToObjCVideoFrameBuffer(frame);
        Require(objc.pixelBuffer == padded && objc.cropX == 100 && objc.cropY == 40, "visible content crop");
        CFRelease(sample);
        CVPixelBufferRelease(padded);
    }
    Require(!tgcalls::MacScreenFrame(nullptr), "invalid sample accepted");
    for (int quality = 0; quality < 12; ++quality) {
        const auto profile = tgcalls::ScreenSharingProfileForQuality(quality);
        const auto size = tgcalls::MacScreenCaptureSize({5120, 2880}, {profile.width, profile.height});
        Require(size.width == (profile.width ? profile.width : 5120) && size.height == (profile.height ? profile.height : 2880), "quality size mismatch");
        const auto small = tgcalls::MacScreenCaptureSize({640, 360}, {profile.width, profile.height});
        Require(small.width == 640 && small.height == 360, "small source upscaled");
    }
    const auto odd = tgcalls::MacScreenCaptureSize({1001, 701}, {});
    Require(odd.width == 1002 && odd.height == 702, "odd source pixels lost");
    puts("PASS: native identity, surface lifetime, nested crop, software fallback, all quality sizes");
}
void EncoderFailureLifetime() {
    // This profile cannot encode 1080p at the requested rate on the tested VT
    // implementation. VT may reject after invoking its output handler inline.
    auto info = [[RTCVideoCodecInfo alloc] initWithName:@"H264"];
    auto encoder = [[TGRTCVideoEncoderH264 alloc] initWithCodecInfo:info];
    auto settings = [[RTCVideoEncoderSettings alloc] init];
    settings.name = @"H264"; settings.width = 1920; settings.height = 1080;
    settings.startBitrate = 30000; settings.maxFramerate = 60;
    settings.mode = RTCVideoCodecModeScreensharing;
    [encoder setCallback:^BOOL(RTCEncodedImage *frame, id<RTCCodecSpecificInfo> specific) { return YES; }];
    Require([encoder startEncodeWithSettings:settings numberOfCores:8] == 0, "failure test startup");
    auto pixel = Pixel(1920, 1080, 0);
    auto surface = (IOSurfaceRef)CFRetain(CVPixelBufferGetIOSurface(pixel));
    NSInteger result = 0;
    @autoreleasepool {
        auto sample = Sample(pixel);
        auto native = tgcalls::MacScreenFrame(sample);
        CFRelease(sample);
        auto frame = [[RTCVideoFrame alloc] initWithBuffer:webrtc::ToObjCVideoFrameBuffer(native)
            rotation:RTCVideoRotation_0 timeStampNs:1000000000];
        result = [encoder encode:frame codecSpecificInfo:nil frameTypes:@[@(RTCFrameTypeVideoFrameKey)]];
    }
    CVPixelBufferRelease(pixel);
    [encoder releaseEncoder];
    Require(IOSurfaceGetUseCount(surface) == 0, "submission failure leaked surface");
    CFRelease(surface);
    printf("PASS: submission result %ld, no double release or surface leak\n", long(result));
}
void Encoder(NSString *codec, int width, int height) {
    auto info = [codec isEqualToString:@"H264"]
        ? [[RTCVideoCodecInfo alloc] initWithName:codec parameters:@{@"profile-level-id": @"42e033", @"packetization-mode": @"1"}]
        : [[RTCVideoCodecInfo alloc] initWithName:codec];
    id<RTCVideoEncoder> encoder = [codec isEqualToString:@"H265"]
        ? (id<RTCVideoEncoder>)[[RTCVideoEncoderH265 alloc] initWithCodecInfo:info]
        : (id<RTCVideoEncoder>)[[TGRTCVideoEncoderH264 alloc] initWithCodecInfo:info];
    auto settings = [[RTCVideoEncoderSettings alloc] init];
    settings.name = codec; settings.width = width; settings.height = height;
    settings.startBitrate = 30000; settings.maxFramerate = 60;
    settings.mode = RTCVideoCodecModeScreensharing;
    std::atomic<int> encoded(0);
    auto count = &encoded;
    [encoder setCallback:^BOOL(RTCEncodedImage *frame, id<RTCCodecSpecificInfo> specific) {
        Require(frame.encodedWidth == width && frame.encodedHeight == height, "encoded resolution");
        count->fetch_add(1);
        return YES;
    }];
    Require([encoder startEncodeWithSettings:settings numberOfCores:8] == 0, "encoder startup");
    IOSurfaceRef surfaces[120] = {};
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 120; ++i) {
        @autoreleasepool {
            auto pixel = Pixel(width, height, i);
            surfaces[i] = (IOSurfaceRef)CFRetain(CVPixelBufferGetIOSurface(pixel));
            auto sample = Sample(pixel);
            auto native = tgcalls::MacScreenFrame(sample);
            CFRelease(sample);
            CVPixelBufferRelease(pixel);
            auto buffer = webrtc::ToObjCVideoFrameBuffer(native);
            auto frame = [[RTCVideoFrame alloc] initWithBuffer:buffer rotation:RTCVideoRotation_0 timeStampNs:(i + 1) * 1000000000LL / 60];
            frame.timeStamp = (i + 1) * 1500;
            const auto result = [encoder encode:frame codecSpecificInfo:nil frameTypes:@[@(i == 0 ? RTCFrameTypeVideoFrameKey : RTCFrameTypeVideoFrameDelta)]];
            if (result != 0) fprintf(stderr, "%s encode result %ld at frame %d\n", codec.UTF8String, long(result), i);
            Require(result == 0, "native encoding");
        }
        std::this_thread::sleep_until(start + std::chrono::microseconds((i + 1) * 1000000LL / 60));
    }
    [encoder releaseEncoder];
    Require(encoded > 0, "no encoded native frames");
    for (auto surface : surfaces) {
        Require(IOSurfaceGetUseCount(surface) == 0, "encoder retained surface lease after release");
        CFRelease(surface);
    }
    printf("PASS: %s native %d x %d, encoded %d / 120 frames, all surface leases released\n", codec.UTF8String, width, height, encoded.load());
}
}
int main(int argc, char **argv) {
    @autoreleasepool {
        BufferAndSizes();
        if (argc > 1) {
            EncoderFailureLifetime();
            Encoder(@"H264", 1920, 1080);
            Encoder(@"H265", 5120, 2880);
        }
    }
}
