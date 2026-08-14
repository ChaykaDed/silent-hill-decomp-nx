#include "PsyX_IdealReverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace PsyX_IdealReverb {

namespace {

constexpr double kFirCoefficients[20] = {
    -1.0 / 32768.0,  2.0 / 32768.0,  -10.0 / 32768.0,  35.0 / 32768.0,
   -103.0 / 32768.0, 266.0 / 32768.0, -616.0 / 32768.0, 1332.0 / 32768.0,
  -2960.0 / 32768.0, 10246.0 / 32768.0, 10246.0 / 32768.0, -2960.0 / 32768.0,
   1332.0 / 32768.0, -616.0 / 32768.0, 266.0 / 32768.0, -103.0 / 32768.0,
     35.0 / 32768.0,  -10.0 / 32768.0,   2.0 / 32768.0,   -1.0 / 32768.0
};
constexpr double kFirCenter = 0.5;

constexpr int16_t S16(uint16_t bits)
{
    return static_cast<int16_t>(bits);
}

// clang-format off
constexpr Preset kPresets[static_cast<int>(PresetId::Count)] = {
    { PresetId::Off, "Off", 0x80, {
        0x0000,0x0000,S16(0x0000),
        S16(0x0000),S16(0x0000),S16(0x0000),S16(0x0000),S16(0x0000),
        S16(0x0000),S16(0x0000),
        0x0001,0x0001,0x0001,0x0001,0x0001,0x0001,
        0x0000,0x0000,0x0001,0x0001,0x0001,0x0001,0x0001,0x0001,
        0x0000,0x0000,0x0001,0x0001,0x0001,0x0001,
        S16(0x0000),S16(0x0000)
    }},
    { PresetId::Room, "Room", 0x26C0, {
        0x007D,0x005B,S16(0x6D80),
        S16(0x54B8),S16(0xBED0),S16(0x0000),S16(0x0000),S16(0xBA80),
        S16(0x5800),S16(0x5300),
        0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,
        0x0334,0x01B5,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x01B4,0x0136,0x00B8,0x005C,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::StudioSmall, "Studio Small", 0x1F40, {
        0x0033,0x0025,S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4410),S16(0xC0F0),S16(0x9C00),
        S16(0x5280),S16(0x4EC0),
        0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,
        0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,
        0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::StudioMedium, "Studio Medium", 0x4840, {
        0x00B1,0x007F,S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0),S16(0xB4C0),
        S16(0x5280),S16(0x4EC0),
        0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,
        0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,
        0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::StudioLarge, "Studio Large", 0x6FE0, {
        0x00E3,0x00A9,S16(0x6F60),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0),S16(0xA680),
        S16(0x5680),S16(0x52C0),
        0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,
        0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,
        0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::Hall, "Hall", 0xADE0, {
        0x01A5,0x0139,S16(0x6000),
        S16(0x5000),S16(0x4C00),S16(0xB800),S16(0xBC00),S16(0xC000),
        S16(0x6000),S16(0x5C00),
        0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
        0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
        0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::SpaceEcho, "Space Echo", 0xF6C0, {
        0x033D,0x0231,S16(0x7E00),
        S16(0x5000),S16(0xB400),S16(0xB000),S16(0x4C00),S16(0xB000),
        S16(0x6000),S16(0x5400),
        0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,
        0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
        0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::ChaosEcho, "Chaos Echo", 0x18040, {
        0x0001,0x0001,S16(0x7FFF),
        S16(0x7FFF),S16(0x0000),S16(0x0000),S16(0x0000),S16(0x8100),
        S16(0x0000),S16(0x0000),
        0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,
        0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x1004,0x1002,0x0004,0x0002,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::Delay, "Delay", 0x18040, {
        0x0001,0x0001,S16(0x7FFF),
        S16(0x7FFF),S16(0x0000),S16(0x0000),S16(0x0000),S16(0x0000),
        S16(0x0000),S16(0x0000),
        0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,
        0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x1004,0x1002,0x0004,0x0002,
        S16(0x8000),S16(0x8000)
    }},
    { PresetId::HalfEcho, "Half Echo", 0x3C00, {
        0x0017,0x0013,S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0),S16(0x8500),
        S16(0x5F80),S16(0x54C0),
        0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,
        0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,
        0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014,
        S16(0x8000),S16(0x8000)
    }}
};
// clang-format on

} // namespace

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53,
              "Ideal reverb requires IEEE 754 binary64");

const Preset& GetPreset(PresetId id)
{
    int index = static_cast<int>(id);
    if (index < 0 || index >= static_cast<int>(PresetId::Count))
        index = static_cast<int>(PresetId::Off);
    return kPresets[index];
}

Reverb::Reverb(uint32_t internalSampleRate)
    : m_storage(kSpuRamHalfwords, 0.0)
{
    SetInternalSampleRate(internalSampleRate);
    Reset();
}

bool Reverb::SetInternalSampleRate(uint32_t sampleRate)
{
    if (sampleRate != kRate176400 && sampleRate != kRate352800)
        return false;
    m_internalSampleRate = sampleRate;
    m_oversampleFactor = sampleRate / kLogicalSampleRate;
    m_internalPhase = 0;
    m_inputAccumulator[0] = 0.0;
    m_inputAccumulator[1] = 0.0;
    m_lastLogicalOutput = {};
    return true;
}

void Reverb::Reset()
{
    m_regs = {};
    ClearDelayStorage();
    m_mBaseRaw = 0;
    m_baseAddress = 0;
    m_currentAddress = 0;
    m_depthLeft = 0;
    m_depthRight = 0;
    m_masterEnable = false;
    m_internalPhase = 0;
    m_inputAccumulator[0] = 0.0;
    m_inputAccumulator[1] = 0.0;
    m_lastLogicalOutput = {};
    std::memset(m_downsampleBuffer, 0, sizeof(m_downsampleBuffer));
    std::memset(m_upsampleBuffer, 0, sizeof(m_upsampleBuffer));
    m_resamplePos = 0;
}

void Reverb::ClearDelayStorage()
{
    std::fill(m_storage.begin(), m_storage.end(), 0.0);
}

void Reverb::ClearDelayState()
{
    ClearDelayStorage();
    m_currentAddress = m_baseAddress;
    m_internalPhase = 0;
    m_inputAccumulator[0] = 0.0;
    m_inputAccumulator[1] = 0.0;
    m_lastLogicalOutput = {};
    std::memset(m_downsampleBuffer, 0, sizeof(m_downsampleBuffer));
    std::memset(m_upsampleBuffer, 0, sizeof(m_upsampleBuffer));
    m_resamplePos = 0;
}

void Reverb::LoadPreset(PresetId id)
{
    LoadPreset(GetPreset(id));
}

void Reverb::LoadPreset(const Preset& preset)
{
    m_regs = preset.regs;
}

void Reverb::SetPsyQParameters(const PsyQParameters& parameters)
{
    int mode = parameters.mode & 0xFF;
    if (mode < 0 || mode >= static_cast<int>(PresetId::Count))
        mode = static_cast<int>(PresetId::Off);
    const Preset& preset = GetPreset(static_cast<PresetId>(mode));
    LoadPreset(preset);
    SetBaseAddress(static_cast<uint16_t>((kSpuRamBytes - preset.workAreaBytes) / 8u));
    SetOutputDepth(parameters.depthLeft, parameters.depthRight);
    SetMasterEnable(parameters.enabled && mode != static_cast<int>(PresetId::Off));
}

void Reverb::SetRegisterRaw(unsigned index, uint16_t value)
{
    if (index >= 32u)
        return;
    std::memcpy(reinterpret_cast<uint8_t*>(&m_regs) + index * sizeof(uint16_t),
                &value, sizeof(value));
}

uint16_t Reverb::GetRegisterRaw(unsigned index) const
{
    if (index >= 32u)
        return 0;
    uint16_t value = 0;
    std::memcpy(&value,
                reinterpret_cast<const uint8_t*>(&m_regs) + index * sizeof(uint16_t),
                sizeof(value));
    return value;
}

void Reverb::SetBaseAddress(uint16_t mBaseRaw)
{
    m_mBaseRaw = mBaseRaw;
    m_baseAddress = (static_cast<uint32_t>(mBaseRaw) << 2) & 0x3FFFFu;
    m_currentAddress = m_baseAddress;
}

void Reverb::SetOutputDepth(int16_t left, int16_t right)
{
    m_depthLeft = left;
    m_depthRight = right;
}

double Reverb::Coefficient(int16_t raw)
{
    return static_cast<double>(raw) * (1.0 / 32768.0);
}

uint32_t Reverb::ResolveHalfwordAddress(uint32_t currentAddress,
                                        uint32_t baseAddress,
                                        uint32_t rawHalfwordOffset)
{
    constexpr uint32_t mask = 0x3FFFFu;
    uint32_t offset = (currentAddress & mask) + (rawHalfwordOffset & mask);
    if (offset & 0x40000u)
        offset += baseAddress & mask;
    return offset & mask;
}

double Reverb::ReverbRead(uint32_t rawFieldValue, int32_t extraHalfwords) const
{
    const uint32_t rawOffset =
        (rawFieldValue << 2) + static_cast<uint32_t>(extraHalfwords);
    return m_storage[ResolveHalfwordAddress(
        m_currentAddress, m_baseAddress, rawOffset)];
}

void Reverb::ReverbWrite(uint32_t rawFieldValue, double value)
{
    const uint32_t rawOffset = rawFieldValue << 2;
    m_storage[ResolveHalfwordAddress(
        m_currentAddress, m_baseAddress, rawOffset)] = value;
}

void Reverb::Process(double inputLeft, double inputRight,
                     double* outputLeft, double* outputRight)
{
    m_inputAccumulator[0] += inputLeft;
    m_inputAccumulator[1] += inputRight;
    ++m_internalPhase;

    if (m_internalPhase == m_oversampleFactor)
    {
        const double scale = 1.0 / static_cast<double>(m_oversampleFactor);
        m_lastLogicalOutput = ProcessLogical44100(
            m_inputAccumulator[0] * scale, m_inputAccumulator[1] * scale);
        m_inputAccumulator[0] = 0.0;
        m_inputAccumulator[1] = 0.0;
        m_internalPhase = 0;
    }

    if (outputLeft)
        *outputLeft = m_lastLogicalOutput.left;
    if (outputRight)
        *outputRight = m_lastLogicalOutput.right;
}

StereoFrame Reverb::Process(double inputLeft, double inputRight)
{
    StereoFrame output{};
    Process(inputLeft, inputRight, &output.left, &output.right);
    return output;
}

StereoFrame Reverb::ProcessLogical44100(double inputLeft, double inputRight)
{
    m_downsampleBuffer[0][m_resamplePos] = inputLeft;
    m_downsampleBuffer[0][m_resamplePos | 0x40u] = inputLeft;
    m_downsampleBuffer[1][m_resamplePos] = inputRight;
    m_downsampleBuffer[1][m_resamplePos | 0x40u] = inputRight;

    double output[2]{};
    if (m_resamplePos & 1u)
    {
        double downsampled[2]{};
        for (int channel = 0; channel < 2; ++channel)
        {
            const double* source =
                &m_downsampleBuffer[channel][(m_resamplePos - 38u) & 0x3Fu];
            double sum = kFirCenter * source[19];
            for (int tap = 0; tap < 20; ++tap)
                sum += kFirCoefficients[tap] * source[tap * 2];
            downsampled[channel] = sum;
        }

        const StereoFrame core = ProcessCore22050(downsampled[0], downsampled[1]);
        m_upsampleBuffer[0][m_resamplePos >> 1] = core.left;
        m_upsampleBuffer[0][(m_resamplePos >> 1) | 0x20u] = core.left;
        m_upsampleBuffer[1][m_resamplePos >> 1] = core.right;
        m_upsampleBuffer[1][(m_resamplePos >> 1) | 0x20u] = core.right;

        for (int channel = 0; channel < 2; ++channel)
        {
            const double* source =
                &m_upsampleBuffer[channel][((m_resamplePos >> 1) - 19u) & 0x1Fu];
            double sum = 0.0;
            for (int tap = 0; tap < 20; ++tap)
                sum += 2.0 * kFirCoefficients[tap] * source[tap];
            output[channel] = sum;
        }
    }
    else
    {
        const size_t index = (((m_resamplePos >> 1) - 19u) & 0x1Fu) + 9u;
        output[0] = m_upsampleBuffer[0][index];
        output[1] = m_upsampleBuffer[1][index];
    }

    m_resamplePos = (m_resamplePos + 1u) & 0x3Fu;
    return {
        output[0] * Coefficient(m_depthLeft),
        output[1] * Coefficient(m_depthRight)
    };
}

StereoFrame Reverb::ProcessCore22050(double inputLeft, double inputRight)
{
    const Registers& r = m_regs;
    const uint16_t mSame[2] = { r.mLSAME, r.mRSAME };
    const uint16_t dSame[2] = { r.dLSAME, r.dRSAME };
    const uint16_t mDiff[2] = { r.mLDIFF, r.mRDIFF };
    const uint16_t dDiff[2] = { r.dLDIFF, r.dRDIFF };
    const uint16_t mComb1[2] = { r.mLCOMB1, r.mRCOMB1 };
    const uint16_t mComb2[2] = { r.mLCOMB2, r.mRCOMB2 };
    const uint16_t mComb3[2] = { r.mLCOMB3, r.mRCOMB3 };
    const uint16_t mComb4[2] = { r.mLCOMB4, r.mRCOMB4 };
    const uint16_t mApf1[2] = { r.mLAPF1, r.mRAPF1 };
    const uint16_t mApf2[2] = { r.mLAPF2, r.mRAPF2 };
    const int16_t inputCoefficient[2] = { r.vLIN, r.vRIN };
    const double input[2] = { inputLeft, inputRight };
    double output[2]{};

    for (int channel = 0; channel < 2; ++channel)
    {
        const int other = channel ^ 1;
        if (m_masterEnable)
        {
            const double iirInputSame =
                ReverbRead(dSame[channel], 0) * Coefficient(r.vWALL) +
                input[channel] * Coefficient(inputCoefficient[channel]);
            const double iirInputDiff =
                ReverbRead(dDiff[other], 0) * Coefficient(r.vWALL) +
                input[channel] * Coefficient(inputCoefficient[channel]);
            const double iir = Coefficient(r.vIIR);
            const double iirSame =
                iirInputSame * iir + ReverbRead(mSame[channel], -1) * (1.0 - iir);
            const double iirDiff =
                iirInputDiff * iir + ReverbRead(mDiff[channel], -1) * (1.0 - iir);
            ReverbWrite(mSame[channel], iirSame);
            ReverbWrite(mDiff[channel], iirDiff);
        }

        const double accumulator =
            ReverbRead(mComb1[channel], 0) * Coefficient(r.vCOMB1) +
            ReverbRead(mComb2[channel], 0) * Coefficient(r.vCOMB2) +
            ReverbRead(mComb3[channel], 0) * Coefficient(r.vCOMB3) +
            ReverbRead(mComb4[channel], 0) * Coefficient(r.vCOMB4);

        const uint32_t apf1Address =
            static_cast<uint32_t>(mApf1[channel]) - static_cast<uint32_t>(r.dAPF1);
        const uint32_t apf2Address =
            static_cast<uint32_t>(mApf2[channel]) - static_cast<uint32_t>(r.dAPF2);
        const double feedbackA = ReverbRead(apf1Address, 0);
        const double feedbackB = ReverbRead(apf2Address, 0);
        const double apf1 = Coefficient(r.vAPF1);
        const double apf2 = Coefficient(r.vAPF2);
        const double destinationA = accumulator - feedbackA * apf1;
        const double destinationB =
            feedbackA + destinationA * apf1 - feedbackB * apf2;
        output[channel] = feedbackB + destinationB * apf2;

        if (m_masterEnable)
        {
            ReverbWrite(mApf1[channel], destinationA);
            ReverbWrite(mApf2[channel], destinationB);
        }
    }

    m_currentAddress = (m_currentAddress + 1u) & 0x3FFFFu;
    if (m_currentAddress == 0u)
        m_currentAddress = m_baseAddress;

    return { output[0], output[1] };
}

double Reverb::ReadStorageHalfword(uint32_t index) const
{
    return m_storage[index & 0x3FFFFu];
}

void Reverb::WriteStorageHalfword(uint32_t index, double value)
{
    m_storage[index & 0x3FFFFu] = value;
}

double Reverb::InternalEnergy() const
{
    double energy = 0.0;
    for (double value : m_storage)
        energy += value * value;
    for (const auto& channel : m_downsampleBuffer)
        for (double value : channel)
            energy += value * value;
    for (const auto& channel : m_upsampleBuffer)
        for (double value : channel)
            energy += value * value;
    return energy;
}

} // namespace PsyX_IdealReverb
