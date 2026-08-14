#include "PsyX_SPUCore.h"
#include "PsyX_audio_convert.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace PsyX;

static void WriteBlock(uint8_t* ram, uint32_t addr, int nibble)
{
    ram[addr] = 0;
    ram[addr + 1] = 7;
    const uint8_t packed = static_cast<uint8_t>((nibble & 15) | ((nibble & 15) << 4));
    for (int i = 0; i < 14; ++i)
        ram[addr + 2 + i] = packed;
}

static void ConfigureVoice(SPUCore& spu, uint16_t pitch)
{
    const uint32_t addr = 0x1000;
    WriteBlock(spu.RamData(), addr, 3);
    SpuVoiceAttr attr{};
    attr.voice = SPU_VOICECH(0);
    attr.mask = 0xFFFFFFFFu;
    attr.addr = addr;
    attr.loop_addr = addr;
    attr.pitch = pitch;
    attr.volume.left = 0x3001;
    attr.volume.right = 0x2801;
    attr.volmode.left = SPU_VOICE_DIRECT16;
    attr.volmode.right = SPU_VOICE_DIRECT16;
    spu.SetVoiceAttrIndexed(0, attr);
    spu.SetMasterVolume(0x3FFF, 0x3FFF);
    spu.SetKey(KeyCmd_OnEnvOff, SPU_VOICECH(0));
}

static void CheckStateParity(RendererMode renderer, int oversample)
{
    auto exact = std::make_unique<SPUCore>();
    auto enhanced = std::make_unique<SPUCore>();
    ConfigureVoice(*exact, 0x1235);
    ConfigureVoice(*enhanced, 0x1235);
    assert(enhanced->SetRenderer(renderer, ClipMode::None));

    int16_t exactOut[32]{};
    std::vector<double> enhancedOut(static_cast<size_t>(16 * oversample) * 2u);
    exact->RenderFrames(exactOut, 16);
    enhanced->RenderFramesDouble(enhancedOut.data(), 16 * oversample);

    const SPUVoiceState& a = exact->GetVoiceState(0);
    const SPUVoiceState& b = enhanced->GetVoiceState(0);
    assert(a.curAddr == b.curAddr);
    assert(a.repeatAddr == b.repeatAddr);
    assert(a.blockSamplePos == b.blockSamplePos);
    assert(a.pitchCounter == b.pitchCounter);
    assert(a.envPhase == b.envPhase);
    assert(a.envLevel == b.envLevel);
    assert(a.envCounter == b.envCounter);
}

static void CheckIntegratedReverb(RendererMode renderer, int oversample)
{
    auto dry = std::make_unique<SPUCore>();
    auto wet = std::make_unique<SPUCore>();
    ConfigureVoice(*dry, 0x1000);
    ConfigureVoice(*wet, 0x1000);
    dry->SetRenderer(renderer, ClipMode::None);
    wet->SetRenderer(renderer, ClipMode::None);
    wet->SetVoiceReverbSend(SPU_VOICECH(0), true);
    wet->SetReverbMasterEnable(true);
    SpuReverbAttr attr{};
    attr.mask = SPU_REV_MODE | SPU_REV_DEPTHL | SPU_REV_DEPTHR;
    attr.mode = 1;
    attr.depth.left = 0x5000;
    attr.depth.right = 0x5000;
    wet->SetReverbModeParam(attr);

    const int frames = 2048 * oversample;
    std::vector<double> dryOut(static_cast<size_t>(frames) * 2u);
    std::vector<double> wetOut(static_cast<size_t>(frames) * 2u);
    dry->RenderFramesDouble(dryOut.data(), frames);
    wet->RenderFramesDouble(wetOut.data(), frames);
    assert(wet->GetEnhancedReverbEnergy() > dry->GetEnhancedReverbEnergy());

    SpuReverbAttr before{};
    wet->GetReverbModeParam(before);
    wet->ClearReverbWorkArea();
    SpuReverbAttr after{};
    wet->GetReverbModeParam(after);
    assert(after.mode == before.mode);
    assert(after.depth.left == before.depth.left);
    assert(after.depth.right == before.depth.right);
    assert(wet->GetReverbMasterEnable());
    assert(wet->GetEnhancedReverbEnergy() == 0.0);
}

static void CheckLiveXaGain(RendererMode renderer)
{
    auto spu = std::make_unique<SPUCore>();
    assert(spu->SetRenderer(renderer, ClipMode::None));
    spu->SetCdAudioEnable(true);
    spu->SetCdVolume(0x4000, 0x4000);
    spu->SetMasterVolume(0x3FFF, 0x3FFF);

    if (renderer == RendererMode::Reference)
    {
        spu->PushCdStereoFrameDouble(12000.0, -8000.0);
        spu->PushCdStereoFrameDouble(12000.0, -8000.0);
        double first[2]{};
        double second[2]{};
        spu->RenderFramesDouble(first, 1);
        spu->SetXaMasterGain(0.25);
        spu->RenderFramesDouble(second, 1);
        assert(std::fabs(second[0] / first[0] - 0.25) < 1.0e-9);
        assert(std::fabs(second[1] / first[1] - 0.25) < 1.0e-9);
    }
    else
    {
        spu->PushCdStereoFrame(12000, -8000);
        spu->PushCdStereoFrame(12000, -8000);
        int16_t first[2]{};
        int16_t second[2]{};
        spu->RenderFrames(first, 1);
        spu->SetXaMasterGain(0.25);
        spu->RenderFrames(second, 1);
        assert(std::abs(second[0] * 4 - first[0]) <= 4);
        assert(std::abs(second[1] * 4 - first[1]) <= 4);
    }

    int16_t cdLeft = 0;
    int16_t cdRight = 0;
    spu->GetCdVolume(&cdLeft, &cdRight);
    assert(cdLeft == 0x4000 && cdRight == 0x4000);
}

int main()
{
    {
        const int16_t taps[4] = {1234, -2345, 3456, -4567};
        const uint32_t counter = 0x7F0;
        const int i = static_cast<int>((counter >> 4) & 255u);
        const int16_t* g = SPUCore::GaussTable();
        const double direct =
            (static_cast<double>(g[0x0FF - i]) * taps[3] +
             static_cast<double>(g[0x1FF - i]) * taps[2] +
             static_cast<double>(g[0x100 + i]) * taps[1] +
             static_cast<double>(g[0x000 + i]) * taps[0]) / 32768.0;
        assert(SPUCore::EvaluateIdealGaussian(taps, counter) == direct);
    }

    {
        SPUCore spu;
        SpuVoiceAttr attr{};
        attr.voice = SPU_VOICECH(0);
        attr.mask = SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2 |
                    SPU_VOICE_ADSR_AMODE;
        attr.adsr1 = 0x1234;
        attr.adsr2 = 0x5678;
        attr.a_mode = 1;
        attr.ar = 0x7F;
        attr.dr = 0xF;
        attr.sl = 0xF;
        attr.sr = 0x7F;
        attr.rr = 0x1F;
        spu.SetVoiceAttr(attr);

        SpuVoiceAttr actual{};
        actual.voice = SPU_VOICECH(0);
        spu.GetVoiceAttr(actual);
        assert(actual.adsr1 == 0x9234);
        assert(actual.adsr2 == 0x5678);
        assert(actual.ar == 0x12);
        assert(actual.dr == 0x3);
        assert(actual.sl == 0x4);
        assert(actual.sr == 0x59);
        assert(actual.rr == 0x18);
    }

    {
        auto a = std::make_unique<SPUCore>();
        auto b = std::make_unique<SPUCore>();
        ConfigureVoice(*a, 0x1000);
        ConfigureVoice(*b, 0x1000);
        assert(b->SetRenderer(RendererMode::Exact, ClipMode::Psx));
        int16_t outA[128]{};
        int16_t outB[128]{};
        a->RenderFrames(outA, 64);
        b->RenderFrames(outB, 64);
        assert(std::memcmp(outA, outB, sizeof(outA)) == 0);
    }

    CheckStateParity(RendererMode::Ideal, 4);
    CheckStateParity(RendererMode::Reference, 8);
    CheckIntegratedReverb(RendererMode::Ideal, 4);
    CheckIntegratedReverb(RendererMode::Reference, 8);
    CheckLiveXaGain(RendererMode::Exact);
    CheckLiveXaGain(RendererMode::Reference);

    {
        auto ideal = std::make_unique<SPUCore>();
        ConfigureVoice(*ideal, 0x1235);
        ideal->SetRenderer(RendererMode::Ideal, ClipMode::None);
        double out[64]{};
        ideal->RenderFramesDouble(out, 32);
        bool fractional = false;
        for (double sample : out)
            fractional |= std::fabs(sample - std::round(sample)) > 1e-9;
        assert(fractional);
        assert(ideal->GetReferenceSincProcessCount() == 0);
    }

    {
        auto reference = std::make_unique<SPUCore>();
        ConfigureVoice(*reference, 0x3000);
        reference->SetRenderer(RendererMode::Reference, ClipMode::None);
        double out[16]{};
        reference->RenderFramesDouble(out, 8);
        assert(reference->GetReferenceSincProcessCount() == 8u);
        assert(reference->GetReferenceLastBandlimitBucket(0) > 0);
    }

    {
        auto reference = std::make_unique<SPUCore>();
        reference->SetRenderer(RendererMode::Reference, ClipMode::None);
        reference->SetCdAudioEnable(true);
        reference->SetCdVolume(0x7FFF, 0x7FFF);
        reference->SetMasterVolume(0x3FFF, 0x3FFF);
        reference->PushCdStereoFrameDouble(1234.5, -2345.25);
        double out[2]{};
        reference->RenderFramesDouble(out, 1);
        assert(out[0] != 0.0 && out[1] != 0.0);
    }

    {
        SPUCore exact;
        SpuReverbAttr attr{};
        attr.mask = SPU_REV_MODE | SPU_REV_DEPTHL | SPU_REV_DEPTHR;
        attr.mode = 1;
        attr.depth.left = 0x3456;
        attr.depth.right = 0x2345;
        exact.SetReverbModeParam(attr);
        const PsyX_SPUReverb::Preset& preset =
            PsyX_SPUReverb::GetPreset(PsyX_SPUReverb::PresetId::Room);
        std::memset(exact.RamData() + kSpuRamSize - preset.workAreaBytes,
                    0x5A, preset.workAreaBytes);
        exact.ClearReverbWorkArea();
        for (uint32_t i = kSpuRamSize - preset.workAreaBytes;
             i < kSpuRamSize; ++i)
            assert(exact.RamData()[i] == 0);
        SpuReverbAttr preserved{};
        exact.GetReverbModeParam(preserved);
        assert(preserved.mode == attr.mode);
        assert(preserved.depth.left == attr.depth.left);
        assert(preserved.depth.right == attr.depth.right);
    }

    {
        using namespace psyx::audio;
        const StereoFrame frame{123.25, -456.75};
        uint8_t a[8]{};
        uint8_t b[8]{};
        uint64_t stateA = 12345;
        uint64_t stateB = 12345;
        packFrame(a, PackedFormat::S32, frame, true, &stateA);
        packFrame(b, PackedFormat::S32, frame, true, &stateB);
        assert(std::memcmp(a, b, sizeof(a)) == 0);
        assert(ReferenceOutputDecimator::StagesForOutputRate(44100) == 3);
        assert(ReferenceOutputDecimator::StagesForOutputRate(48000) == -1);
    }

    std::puts("All enhanced SPU integration tests passed.");
    return 0;
}
