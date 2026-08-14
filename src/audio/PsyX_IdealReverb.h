#ifndef PSYX_IDEALREVERB_H
#define PSYX_IDEALREVERB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PsyX_IdealReverb {

constexpr uint32_t kSpuRamBytes = 512u * 1024u;
constexpr uint32_t kSpuRamHalfwords = kSpuRamBytes / 2u;
constexpr uint32_t kWorkAreaEndAddress = 0x7FFFEu;
constexpr uint32_t kLogicalSampleRate = 44100u;
constexpr uint32_t kReverbSampleRate = 22050u;
constexpr uint32_t kRate176400 = 176400u;
constexpr uint32_t kRate352800 = 352800u;

struct Registers
{
    uint16_t dAPF1, dAPF2;
    int16_t vIIR;
    int16_t vCOMB1, vCOMB2, vCOMB3, vCOMB4;
    int16_t vWALL;
    int16_t vAPF1, vAPF2;
    uint16_t mLSAME, mRSAME;
    uint16_t mLCOMB1, mRCOMB1;
    uint16_t mLCOMB2, mRCOMB2;
    uint16_t dLSAME, dRSAME;
    uint16_t mLDIFF, mRDIFF;
    uint16_t mLCOMB3, mRCOMB3;
    uint16_t mLCOMB4, mRCOMB4;
    uint16_t dLDIFF, dRDIFF;
    uint16_t mLAPF1, mRAPF1;
    uint16_t mLAPF2, mRAPF2;
    int16_t vLIN, vRIN;
};

static_assert(sizeof(Registers) == 32u * sizeof(uint16_t),
              "PSX reverb register block must contain exactly 32 halfwords");

enum class PresetId : int
{
    Off = 0,
    Room,
    StudioSmall,
    StudioMedium,
    StudioLarge,
    Hall,
    SpaceEcho,
    ChaosEcho,
    Delay,
    HalfEcho,
    Count
};

struct Preset
{
    PresetId id;
    const char* name;
    uint32_t workAreaBytes;
    Registers regs;
};

struct PsyQParameters
{
    int mode = static_cast<int>(PresetId::Room);
    int16_t depthLeft = 0;
    int16_t depthRight = 0;
    bool enabled = true;
};

struct StereoFrame
{
    double left;
    double right;
};

const Preset& GetPreset(PresetId id);

class Reverb
{
public:
    explicit Reverb(uint32_t internalSampleRate = kRate176400);

    bool SetInternalSampleRate(uint32_t sampleRate);
    uint32_t GetInternalSampleRate() const { return m_internalSampleRate; }
    uint32_t GetOversampleFactor() const { return m_oversampleFactor; }

    void Reset();
    void ClearDelayStorage();
    void ClearDelayState();

    void LoadPreset(PresetId id);
    void LoadPreset(const Preset& preset);
    void SetPsyQParameters(const PsyQParameters& parameters);

    void SetRegisters(const Registers& regs) { m_regs = regs; }
    const Registers& GetRegisters() const { return m_regs; }
    void SetRegisterRaw(unsigned index, uint16_t value);
    uint16_t GetRegisterRaw(unsigned index) const;

    void SetBaseAddress(uint16_t mBaseRaw);
    uint16_t GetBaseAddressRaw() const { return m_mBaseRaw; }
    uint32_t GetBaseAddressHalfword() const { return m_baseAddress; }
    uint32_t GetCurrentAddressHalfword() const { return m_currentAddress; }

    void SetOutputDepth(int16_t left, int16_t right);
    int16_t GetOutputDepthLeft() const { return m_depthLeft; }
    int16_t GetOutputDepthRight() const { return m_depthRight; }

    void SetMasterEnable(bool enabled) { m_masterEnable = enabled; }
    bool GetMasterEnable() const { return m_masterEnable; }

    void Process(double inputLeft, double inputRight, double* outputLeft, double* outputRight);
    StereoFrame Process(double inputLeft, double inputRight);

    double ReadStorageHalfword(uint32_t index) const;
    void WriteStorageHalfword(uint32_t index, double value);
    double InternalEnergy() const;

    static uint32_t ResolveHalfwordAddress(uint32_t currentAddress,
                                           uint32_t baseAddress,
                                           uint32_t rawHalfwordOffset);

private:
    static double Coefficient(int16_t raw);
    double ReverbRead(uint32_t rawFieldValue, int32_t extraHalfwords) const;
    void ReverbWrite(uint32_t rawFieldValue, double value);
    StereoFrame ProcessLogical44100(double inputLeft, double inputRight);
    StereoFrame ProcessCore22050(double inputLeft, double inputRight);

    Registers m_regs{};
    std::vector<double> m_storage;
    uint32_t m_internalSampleRate = kRate176400;
    uint32_t m_oversampleFactor = 4;
    uint32_t m_internalPhase = 0;
    double m_inputAccumulator[2]{};
    StereoFrame m_lastLogicalOutput{};

    uint16_t m_mBaseRaw = 0;
    uint32_t m_baseAddress = 0;
    uint32_t m_currentAddress = 0;
    int16_t m_depthLeft = 0;
    int16_t m_depthRight = 0;
    bool m_masterEnable = false;

    double m_downsampleBuffer[2][128]{};
    double m_upsampleBuffer[2][64]{};
    uint32_t m_resamplePos = 0;
};

} // namespace PsyX_IdealReverb

#endif
