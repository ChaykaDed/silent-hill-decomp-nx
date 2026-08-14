#include "../../src/audio/PsyX_ReferenceReverb.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace PsyX_ReferenceReverb;

static Reverb MakeRoom()
{
    Reverb reverb;
    PsyQParameters parameters;
    parameters.mode = static_cast<int>(PsyQMode::Room);
    parameters.depthLeft = 32767;
    parameters.depthRight = 32767;
    reverb.SetPsyQParameters(parameters);
    reverb.Reset();
    return reverb;
}

static void TestSilence()
{
    Reverb reverb = MakeRoom();
    for (int i = 0; i < kSampleRate; ++i)
    {
        double left = 1.0;
        double right = 1.0;
        reverb.Process(0.0, 0.0, &left, &right);
        assert(left == 0.0);
        assert(right == 0.0);
    }
    assert(reverb.InternalEnergy() == 0.0);
    std::printf("[OK] silence\n");
}

static void TestImpulseDecay()
{
    Reverb reverb = MakeRoom();
    double earlyEnergy = 0.0;
    double lateEnergy = 0.0;
    double finalEnergy = 0.0;

    for (int i = 0; i < 7 * kSampleRate; ++i)
    {
        const double impulse = i == 0 ? 1.0 : 0.0;
        double left;
        double right;
        reverb.Process(impulse, impulse, &left, &right);
        const double energy = left * left + right * right;
        if (i >= kSampleRate / 10 && i < kSampleRate)
            earlyEnergy += energy;
        if (i >= 3 * kSampleRate && i < 4 * kSampleRate)
            lateEnergy += energy;
        if (i >= 6 * kSampleRate)
            finalEnergy += energy;
    }

    assert(earlyEnergy > 1.0e-6);
    assert(lateEnergy > 0.0);
    assert(lateEnergy < earlyEnergy * 0.01);
    assert(finalEnergy < lateEnergy * 0.01);
    std::printf("[OK] impulse decay\n");
}

static void TestStability()
{
    Reverb reverb;
    PsyQParameters parameters;
    parameters.mode = static_cast<int>(PsyQMode::SpaceEcho);
    parameters.depthLeft = 32767;
    parameters.depthRight = 32767;
    parameters.delay = 127;
    parameters.feedback = 127;
    reverb.SetPsyQParameters(parameters);
    reverb.Reset();

    double peak = 0.0;
    uint32_t random = 0x12345678u;
    for (int i = 0; i < 12 * kSampleRate; ++i)
    {
        random = random * 1664525u + 1013904223u;
        const double input = i < 2 * kSampleRate
            ? (static_cast<double>((random >> 8) & 0xFFFFu) / 32767.5 - 1.0)
            : 0.0;
        double left;
        double right;
        reverb.Process(input, -input * 0.75, &left, &right);
        assert(std::isfinite(left));
        assert(std::isfinite(right));
        if ((i & 4095) == 0)
            assert(std::isfinite(reverb.InternalEnergy()));
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
    }

    assert(peak < 4.0);
    assert(reverb.InternalEnergy() < 2.0);
    std::printf("[OK] stability\n");
}

static void TestStereoDecorrelation()
{
    Reverb reverb = MakeRoom();
    double sumLeftSquared = 0.0;
    double sumRightSquared = 0.0;
    double sumCross = 0.0;
    double differenceEnergy = 0.0;

    for (int i = 0; i < 3 * kSampleRate; ++i)
    {
        const double impulse = i == 0 ? 1.0 : 0.0;
        double left;
        double right;
        reverb.Process(impulse, impulse, &left, &right);
        if (i > kSampleRate / 20)
        {
            sumLeftSquared += left * left;
            sumRightSquared += right * right;
            sumCross += left * right;
            const double difference = left - right;
            differenceEnergy += difference * difference;
        }
    }

    const double correlation = sumCross / std::sqrt(sumLeftSquared * sumRightSquared);
    assert(sumLeftSquared > 0.0);
    assert(sumRightSquared > 0.0);
    assert(std::abs(correlation) < 0.8);
    assert(differenceEnergy > (sumLeftSquared + sumRightSquared) * 0.1);
    std::printf("[OK] stereo decorrelation\n");
}

static void TestDeterministicOutput()
{
    Reverb first = MakeRoom();
    Reverb second = MakeRoom();

    for (int i = 0; i < 2 * kSampleRate; ++i)
    {
        const double inputLeft = (i % 997 == 0) ? 0.75 : 0.0;
        const double inputRight = (i % 1597 == 0) ? -0.5 : 0.0;
        double firstLeft;
        double firstRight;
        double secondLeft;
        double secondRight;
        first.Process(inputLeft, inputRight, &firstLeft, &firstRight);
        second.Process(inputLeft, inputRight, &secondLeft, &secondRight);
        assert(std::memcmp(&firstLeft, &secondLeft, sizeof(double)) == 0);
        assert(std::memcmp(&firstRight, &secondRight, sizeof(double)) == 0);
    }
    std::printf("[OK] deterministic output\n");
}

static void TestBoundedEnergy()
{
    Reverb reverb = MakeRoom();
    double outputEnergy = 0.0;
    double peakInternalEnergy = 0.0;

    for (int i = 0; i < 10 * kSampleRate; ++i)
    {
        const double input = i < 256 ? 1.0 : 0.0;
        double left;
        double right;
        reverb.Process(input, input, &left, &right);
        outputEnergy += left * left + right * right;
        if ((i & 1023) == 0)
            peakInternalEnergy = std::max(peakInternalEnergy, reverb.InternalEnergy());
    }

    assert(outputEnergy < 256.0);
    assert(peakInternalEnergy < 2.0);
    assert(reverb.InternalEnergy() < 1.0e-8);
    std::printf("[OK] bounded energy\n");
}

static void TestPsyQMappingAndVoiceSends()
{
    PsyQParameters roomParameters;
    roomParameters.mode = static_cast<int>(PsyQMode::Room);
    roomParameters.depthLeft = 0x4000;
    roomParameters.depthRight = 0x2000;
    const RoomParameters room = Reverb::MapPsyQParameters(roomParameters);
    assert(room.decaySeconds > 1.5 && room.decaySeconds < 2.2);
    assert(room.preDelaySeconds > 0.01 && room.preDelaySeconds < 0.03);
    assert(room.wetLeft > room.wetRight);

    Reverb reverb;
    reverb.SetVoiceSend(3, true, 0.5);
    assert(reverb.GetVoiceSendTarget(3) == 0.25);
    reverb.SetVoiceSend(3, false);
    assert(reverb.GetVoiceSendTarget(3) == 0.0);
    std::printf("[OK] PsyQ mapping / per-voice sends\n");
}

int main()
{
    TestSilence();
    TestImpulseDecay();
    TestStability();
    TestStereoDecorrelation();
    TestDeterministicOutput();
    TestBoundedEnergy();
    TestPsyQMappingAndVoiceSends();
    std::printf("All PsyX reference reverb tests passed.\n");
    return 0;
}
