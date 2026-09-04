/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace RoundVideoData {

constexpr auto kLogoSize = 80;

struct LogoRLENode final {
	uint16_t count;
	uint8_t value;
};
using LogoRLEFrame = std::vector<LogoRLENode>;

void DecompressLogoRLEFrame(
		const std::vector<LogoRLENode> &rleFrame,
		uint8_t outFrame[kLogoSize][kLogoSize]) {
	auto pos = size_t(0);
	for (const auto &node : rleFrame) {
		for (auto i = uint16_t(0); i < node.count; ++i) {
			if (pos >= kLogoSize * kLogoSize) {
				break;
			}
			const auto y = int(pos / kLogoSize);
			const auto x = int(pos % kLogoSize);
			outFrame[y][x] = node.value;
			pos++;
		}
	}
}

// Upstream burns an animated Telegram logo into every round video message.
// FoxMes carries no mark there - only the circular caption around the frame -
// so the mask is a single empty frame: the blend below adds zero everywhere and
// the machinery stays in place, ready for a real mark if one is ever wanted.
const auto kLogoFrames = std::array<LogoRLEFrame, 1>{ {

	LogoRLEFrame{ { kLogoSize * kLogoSize, 0 } },

} };

} // namespace RoundVideoData
