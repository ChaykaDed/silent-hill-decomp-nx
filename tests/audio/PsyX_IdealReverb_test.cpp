// Standalone build:
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic
//     ../../pc_port/PsyCross/src/audio/PsyX_IdealReverb.cpp
//     PsyX_IdealReverb_test.cpp -o ideal_reverb_test

#include "../../src/audio/PsyX_IdealReverb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace PsyX_IdealReverb;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition) \
    do { \
        ++g_checks; \
        if (!(condition)) { \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++g_failures; \
        } \
    } while (0)

bool Near(double a, double b, double tolerance = 1.0e-12)
{
    return std::abs(a - b) <= tolerance *
        std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

Registers MakeDirectTopology()
{
    Registers r{};
    r.vIIR = 0x4000;
    r.vCOMB1 = 0x7FFF;
    r.vLIN = static_cast<int16_t>(0x8000);
    r.vRIN = static_cast<int16_t>(0x8000);

    r.mLSAME = 4;
    r.mRSAME = 8;
    r.mLCOMB1 = r.mLSAME;
    r.mRCOMB1 = r.mRSAME;
    r.mLDIFF = 12;
    r.mRDIFF = 16;
    r.mLAPF1 = r.mLSAME;
    r.mRAPF1 = r.mRSAME;
    r.mLAPF2 = r.mLSAME;
    r.mRAPF2 = r.mRSAME;
    return r;
}

void Configure(Reverb& reverb, const Registers& registers,
               int16_t depthLeft = 0x7FFF, int16_t depthRight = 0x7FFF)
{
    reverb.SetRegisters(registers);
    reverb.SetBaseAddress(0xF000);
    reverb.SetOutputDepth(depthLeft, depthRight);
    reverb.SetMasterEnable(true);
}

std::vector<StereoFrame> RenderImpulse(uint32_t rate, const Registers& registers,
                                       int frames, double left, double right)
{
    Reverb reverb(rate);
    Configure(reverb, registers);
    std::vector<StereoFrame> output;
    output.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        output.push_back(reverb.Process(i == 0 ? left : 0.0, i == 0 ? right : 0.0));
    return output;
}

size_t FirstNonzero(const std::vector<StereoFrame>& frames, bool right)
{
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const double value = right ? frames[i].right : frames[i].left;
        if (value != 0.0)
            return i;
    }
    return frames.size();
}

void TestSilence()
{
    for (uint32_t rate : { kRate176400, kRate352800 })
    {
        Reverb reverb(rate);
        PsyQParameters parameters;
        parameters.mode = static_cast<int>(PresetId::Room);
        parameters.depthLeft = 0x6000;
        parameters.depthRight = 0x6000;
        reverb.SetPsyQParameters(parameters);
        for (int i = 0; i < 100000; ++i)
        {
            const StereoFrame frame = reverb.Process(0.0, 0.0);
            CHECK(frame.left == 0.0);
            CHECK(frame.right == 0.0);
        }
        CHECK(reverb.InternalEnergy() == 0.0);
    }
}

void TestRatesAndCadence()
{
    Reverb reverb;
    CHECK(reverb.GetInternalSampleRate() == kRate176400);
    CHECK(reverb.GetOversampleFactor() == 4);
    CHECK(!reverb.SetInternalSampleRate(44100));
    CHECK(reverb.GetInternalSampleRate() == kRate176400);
    CHECK(reverb.SetInternalSampleRate(kRate352800));
    CHECK(reverb.GetOversampleFactor() == 8);

    const Registers direct = MakeDirectTopology();
    const auto four = RenderImpulse(kRate176400, direct, 5000, 32768.0, 0.0);
    const auto eight = RenderImpulse(kRate352800, direct, 10000, 32768.0, 0.0);
    const size_t firstFour = FirstNonzero(four, false);
    const size_t firstEight = FirstNonzero(eight, false);
    CHECK(firstFour < four.size());
    CHECK(firstEight < eight.size());
    CHECK(firstFour == 79u);
    CHECK(firstEight == 159u);
    CHECK(firstEight == firstFour * 2u + 1u);

    for (size_t i = 1; i < four.size(); ++i)
        if ((i & 3u) != 3u)
            CHECK(four[i].left == four[i - 1].left);
    for (size_t i = 1; i < eight.size(); ++i)
        if ((i & 7u) != 7u)
            CHECK(eight[i].left == eight[i - 1].left);

    Reverb cadenceFour(kRate176400);
    Reverb cadenceEight(kRate352800);
    Configure(cadenceFour, direct);
    Configure(cadenceEight, direct);
    for (int i = 0; i < 4000; ++i)
        cadenceFour.Process(0.0, 0.0);
    for (int i = 0; i < 8000; ++i)
        cadenceEight.Process(0.0, 0.0);
    CHECK(cadenceFour.GetCurrentAddressHalfword() ==
          cadenceEight.GetCurrentAddressHalfword());
}

void TestImpulseTopologyAndStereo()
{
    Registers r = MakeDirectTopology();
    r.vWALL = 0x4000;
    r.dRDIFF = r.mLSAME;
    r.mRCOMB2 = r.mRDIFF;
    r.vCOMB2 = 0x4000;

    const auto output = RenderImpulse(kRate176400, r, 50000, 32768.0, 0.0);
    const size_t firstLeft = FirstNonzero(output, false);
    const size_t firstRight = FirstNonzero(output, true);
    CHECK(firstLeft < output.size());
    CHECK(firstRight < output.size());
    CHECK(firstRight > firstLeft);

    bool differs = false;
    bool sawFraction = false;
    for (const StereoFrame& frame : output)
    {
        CHECK(std::isfinite(frame.left));
        CHECK(std::isfinite(frame.right));
        differs = differs || frame.left != frame.right;
        sawFraction = sawFraction ||
            (frame.left != 0.0 && std::floor(frame.left) != frame.left);
    }
    CHECK(differs);
    CHECK(sawFraction);
}

void TestDecay()
{
    Reverb reverb(kRate176400);
    PsyQParameters parameters;
    parameters.mode = static_cast<int>(PresetId::Room);
    parameters.depthLeft = 0x7FFF;
    parameters.depthRight = 0x7FFF;
    reverb.SetPsyQParameters(parameters);

    double early = 0.0;
    double late = 0.0;
    bool heard = false;
    for (int i = 0; i < 400000; ++i)
    {
        const StereoFrame frame = reverb.Process(i == 0 ? 32768.0 : 0.0,
                                                 i == 0 ? 32768.0 : 0.0);
        const double magnitude = std::abs(frame.left) + std::abs(frame.right);
        if (i >= 20000 && i < 100000)
            early += magnitude;
        if (i >= 300000)
            late += magnitude;
        heard = heard || magnitude != 0.0;
    }
    CHECK(heard);
    CHECK(early > 0.0);
    CHECK(late < early);
}

void TestDeterminismAndDepth()
{
    Reverb full(kRate176400);
    Reverb half(kRate176400);
    Reverb repeat(kRate176400);
    const Registers registers = MakeDirectTopology();
    Configure(full, registers, 0x7FFF, static_cast<int16_t>(0x8000));
    Configure(half, registers, 0x4000, 0x4000);
    Configure(repeat, registers, 0x7FFF, static_cast<int16_t>(0x8000));

    for (int i = 0; i < 30000; ++i)
    {
        const double left = (i % 997 == 0) ? 12345.6789012345 : 0.0;
        const double right = (i % 619 == 0) ? -9876.5432109876 : 0.0;
        const StereoFrame a = full.Process(left, right);
        const StereoFrame b = half.Process(left, right);
        const StereoFrame c = repeat.Process(left, right);
        CHECK(a.left == c.left);
        CHECK(a.right == c.right);
        if (a.left != 0.0)
            CHECK(Near(b.left, a.left * (16384.0 / 32767.0)));
        if (a.right != 0.0)
            CHECK(Near(b.right, a.right * (-16384.0 / 32768.0)));
    }
}

void TestNoFixedPointOverflowOrSaturation()
{
    Reverb reverb(kRate352800);
    Configure(reverb, MakeDirectTopology());
    bool exceededS16 = false;
    bool retainedFraction = false;
    for (int i = 0; i < 100000; ++i)
    {
        const double input = (i < 8) ? 1.0e12 + 0.25 : 0.0;
        const StereoFrame frame = reverb.Process(input, -input);
        CHECK(std::isfinite(frame.left));
        CHECK(std::isfinite(frame.right));
        exceededS16 = exceededS16 ||
            std::abs(frame.left) > 32768.0 || std::abs(frame.right) > 32768.0;
        retainedFraction = retainedFraction ||
            (frame.left != 0.0 && frame.left != std::trunc(frame.left));
    }
    CHECK(exceededS16);
    CHECK(retainedFraction);
    CHECK(std::isfinite(reverb.InternalEnergy()));
}

void TestPresetDepthMapping()
{
    const uint32_t sizes[] = {
        0x80, 0x26C0, 0x1F40, 0x4840, 0x6FE0,
        0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00
    };
    for (int i = 0; i < static_cast<int>(PresetId::Count); ++i)
    {
        const Preset& preset = GetPreset(static_cast<PresetId>(i));
        CHECK(static_cast<int>(preset.id) == i);
        CHECK(preset.workAreaBytes == sizes[i]);
        CHECK(preset.name != nullptr && preset.name[0] != '\0');
    }
    CHECK(GetPreset(static_cast<PresetId>(999)).id == PresetId::Off);

    Reverb reverb;
    PsyQParameters parameters;
    parameters.mode = static_cast<int>(PresetId::Hall);
    parameters.depthLeft = 0x1234;
    parameters.depthRight = static_cast<int16_t>(0xFEDC);
    parameters.enabled = true;
    reverb.SetPsyQParameters(parameters);
    CHECK(std::memcmp(&reverb.GetRegisters(), &GetPreset(PresetId::Hall).regs,
                      sizeof(Registers)) == 0);
    CHECK(reverb.GetBaseAddressRaw() ==
          static_cast<uint16_t>((kSpuRamBytes - GetPreset(PresetId::Hall).workAreaBytes) / 8u));
    CHECK(reverb.GetOutputDepthLeft() == parameters.depthLeft);
    CHECK(reverb.GetOutputDepthRight() == parameters.depthRight);
    CHECK(reverb.GetMasterEnable());

    parameters.mode = static_cast<int>(PresetId::Off);
    reverb.SetPsyQParameters(parameters);
    CHECK(!reverb.GetMasterEnable());
}

void TestRegisterRoundtrip()
{
    Reverb reverb;
    uint16_t expected[32]{};
    for (unsigned i = 0; i < 32u; ++i)
    {
        expected[i] = static_cast<uint16_t>(0x8001u + i * 0x491u);
        reverb.SetRegisterRaw(i, expected[i]);
    }
    for (unsigned i = 0; i < 32u; ++i)
        CHECK(reverb.GetRegisterRaw(i) == expected[i]);

    const Registers before = reverb.GetRegisters();
    reverb.SetRegisterRaw(32, 0xFFFF);
    CHECK(std::memcmp(&before, &reverb.GetRegisters(), sizeof(before)) == 0);
    CHECK(reverb.GetRegisterRaw(32) == 0);
}

void TestAddressWrap()
{
    CHECK(Reverb::ResolveHalfwordAddress(0x3FFFE, 0x3FFF0, 4) == 0x3FFF2);
    CHECK(Reverb::ResolveHalfwordAddress(0x3FFF0, 0x3FFF0, 3) == 0x3FFF3);
    CHECK(Reverb::ResolveHalfwordAddress(0, 0, 0xFFFFFFFFu) == 0x3FFFF);

    Reverb reverb(kRate176400);
    Configure(reverb, MakeDirectTopology());
    reverb.SetBaseAddress(0xFFFC);
    const uint32_t base = reverb.GetBaseAddressHalfword();
    CHECK(base == 0x3FFF0);
    for (int i = 0; i < 16 * 2 * 4; ++i)
        reverb.Process((i == 0) ? 1.0 : 0.0, 0.0);
    CHECK(reverb.GetCurrentAddressHalfword() == base);

    reverb.WriteStorageHalfword(0x40003, 123.25);
    CHECK(reverb.ReadStorageHalfword(3) == 123.25);
}

} // namespace

int main()
{
    TestSilence();
    TestRatesAndCadence();
    TestImpulseTopologyAndStereo();
    TestDecay();
    TestDeterminismAndDepth();
    TestNoFixedPointOverflowOrSaturation();
    TestPresetDepthMapping();
    TestRegisterRoundtrip();
    TestAddressWrap();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d/%d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("All %d PsyX_IdealReverb checks passed\n", g_checks);
    return 0;
}
