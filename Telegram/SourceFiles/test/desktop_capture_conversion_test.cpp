#include "tgcalls/desktop_capturer/DesktopCaptureConversion.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
void Require(bool value) {
    if (!value) std::abort();
}

void Test(int sourceWidth, int sourceHeight, int width, int height) {
    const auto sourceStride = sourceWidth * 4 + 64;
    const auto stride = width * 4 + 64;
    auto source = std::vector<uint8_t>(sourceStride * sourceHeight);
    for (auto i = size_t(0); i < source.size(); ++i) {
        source[i] = (i * 17 + i / 253) & 255;
    }
    auto serial = std::vector<uint8_t>(stride * height);
    auto parallel = serial;
    tgcalls::DesktopCaptureConversion converter;
    Require(libyuv::ARGBScale(source.data(), sourceStride, sourceWidth, sourceHeight,
        serial.data(), stride, width, height, libyuv::kFilterBilinear) == 0);
    Require(converter.scale(source.data(), sourceStride, sourceWidth, sourceHeight,
        parallel.data(), stride, width, height) == 0);
    Require(serial == parallel);
    const auto yStride = width + 32;
    const auto uvStride = (width + 1) / 2 + 16;
    const auto ySize = yStride * height;
    const auto uvSize = uvStride * ((height + 1) / 2);
    auto expected = std::vector<uint8_t>(ySize + 2 * uvSize);
    auto actual = expected;
    auto oldConvert = [&] {
        return libyuv::ARGBToI420(serial.data(), stride, expected.data(), yStride,
            expected.data() + ySize, uvStride, expected.data() + ySize + uvSize,
            uvStride, width, height);
    };
    auto convert = [&] {
        return converter.convert(parallel.data(), stride, actual.data(), yStride,
            actual.data() + ySize, uvStride, actual.data() + ySize + uvSize,
            uvStride, width, height);
    };
    Require(oldConvert() == 0 && convert() == 0 && expected == actual);
    const auto benchmark = [&](bool useParallel) {
        const auto start = std::chrono::steady_clock::now();
        for (auto i = 0; i < 40; ++i) {
            if (useParallel) {
                Require(converter.scale(source.data(), sourceStride, sourceWidth, sourceHeight,
                    parallel.data(), stride, width, height) == 0 && convert() == 0);
            } else {
                Require(libyuv::ARGBScale(source.data(), sourceStride, sourceWidth, sourceHeight,
                    serial.data(), stride, width, height, libyuv::kFilterBilinear) == 0 && oldConvert() == 0);
            }
        }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 40;
    };
    const auto before = benchmark(false);
    const auto after = benchmark(true);
    Require(serial == parallel && expected == actual);
    std::cout << sourceWidth << 'x' << sourceHeight << " -> " << width << 'x' << height
        << ": serial " << before << " ms, parallel " << after << " ms; pixels identical\n";
}
}

int main() {
    Test(3840, 2160, 2560, 1440);
    Test(3840, 2160, 1920, 1080);
    Test(2560, 1440, 1280, 720);
    Test(1920, 1080, 1920, 1080);
    Test(800, 600, 640, 480);
    Test(832, 479, 800, 477);
}
