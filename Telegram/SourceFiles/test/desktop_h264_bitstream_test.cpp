#include "tgcalls/platform/tdesktop/DesktopH264Bitstream.h"
#include <cstdlib>
#include <iostream>
#define REQUIRE(value) do { if (!(value)) { std::cerr << #value << "\n"; std::abort(); } } while (false)
#include <vector>

int main() {
    using namespace tgcalls;
    std::vector<uint8_t> output;
    const uint8_t annex[] = {0,0,0,1,0x65,1,2};
    REQUIRE(H264ToAnnexB(annex, sizeof(annex), 4, output));
    REQUIRE(output == std::vector<uint8_t>(annex, annex + sizeof(annex)));
    const uint8_t avc[] = {0,0,0,3,0x65,1,2,0,0,0,2,0x41,3};
    REQUIRE(H264ToAnnexB(avc, sizeof(avc), 4, output));
    REQUIRE(output == std::vector<uint8_t>({0,0,0,1,0x65,1,2,0,0,0,1,0x41,3}));
    REQUIRE(!H264ToAnnexB(avc, sizeof(avc) - 1, 4, output));
    REQUIRE(!H264ToAnnexB(avc, sizeof(avc), 0, output));
    const uint8_t oneByte[] = {2,0x65,1};
    REQUIRE(H264ToAnnexB(oneByte, sizeof(oneByte), 1, output));
    REQUIRE(output == std::vector<uint8_t>({0,0,0,1,0x65,1}));
    const uint8_t zeroLength[] = {0,0,0,0};
    REQUIRE(!H264ToAnnexB(zeroLength, sizeof(zeroLength), 4, output));
    const uint8_t config[] = {1,66,224,31,255,225,0,4,0x67,66,224,31,1,0,2,0x68,1};
    int length = 0;
    REQUIRE(H264ParameterSets(config, sizeof(config), length, output));
    REQUIRE(length == 4);
    REQUIRE(output == std::vector<uint8_t>({0,0,0,1,0x67,66,224,31,0,0,0,1,0x68,1}));
    REQUIRE(!H264ParameterSets(config, sizeof(config) - 1, length, output));
    REQUIRE(H264ParameterSets(annex, sizeof(annex), length, output));
    REQUIRE(!H264ParameterSets(avc, sizeof(avc), length, output));
    REQUIRE(H264ParameterSets(nullptr, 0, length, output));
}
