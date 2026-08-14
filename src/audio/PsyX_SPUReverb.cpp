#include "PsyX_SPUReverb.h"

#include <cstring>

namespace PsyX_SPUReverb {

namespace {

// psx-spx "Reverb Buffer Resampling": full 39-tap FIR filter is
//  -0001h, 0000h, 0002h, 0000h, -000Ah, 0000h, 0023h, 0000h,
//  -0067h, 0000h, 010Ah, 0000h, -0268h, 0000h, 0534h, 0000h,
//  -0B90h, 0000h, 2806h, 4000h, 2806h, 0000h, -0B90h, 0000h,
//   0534h, 0000h, -0268h, 0000h, 010Ah, 0000h, -0067h, 0000h,
//   0023h, 0000h, -000Ah, 0000h, 0002h, 0000h, -0001h
// This is a symmetric half-band filter: every tap at an odd index is exactly
// zero *except* the true centre tap (index 19, value 4000h). The 20 nonzero
// even-index taps are kept here in order; the centre tap is folded in
// separately wherever it is needed (see kFirCenterTap below).
constexpr int32_t kFirCoeffs[20] = {
    -0x0001, 0x0002,  -0x000A, 0x0023,  -0x0067, 0x010A,  -0x0268, 0x0534,
    -0x0B90, 0x2806,   0x2806, -0x0B90,  0x0534, -0x0268,  0x010A, -0x0067,
     0x0023, -0x000A,  0x0002, -0x0001,
};
constexpr int32_t kFirCenterTap = 0x4000;

constexpr int32_t Clamp16(int32_t v)
{
    return (v < -0x8000) ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v);
}

constexpr int16_t Truncate16(int32_t v)
{
    // Wrap (NOT saturate) to 16 bits, matching DuckStation's Truncate16 used
    // for last_reverb_input / all reverb-buffer writes. This is intentionally
    // different from Clamp16: register writes derived from already-clamped
    // arithmetic use Truncate16 purely as a narrowing cast (safe, no data
    // loss because the value is already in range), but the initial mixer
    // input truncation genuinely relies on 2's-complement wraparound rather
    // than saturation.
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(v)));
}

constexpr int32_t ApplyVolume(int32_t sample, int16_t volume)
{
    return (sample * static_cast<int32_t>(volume)) >> 15;
}

// psx-spx "Bug": "vIIR works only in range -7FFFh..+7FFFh. When set to
// -8000h, the multiplication by -8000h is still done correctly, but, the
// final result (the value written to memory) gets negated". Reproduced
// exactly as DuckStation's `iiasm` lambda (itself from Mednafen-PSX).
int32_t ReflectFeedback(int16_t oldSample, int16_t vIIR)
{
    if (vIIR == -0x8000)
        return (oldSample == -0x8000) ? 0 : (static_cast<int32_t>(oldSample) * -65536);
    return static_cast<int32_t>(oldSample) * (0x8000 - static_cast<int32_t>(vIIR));
}

// Negation used by the APF stages; avoids overflowing s16 range when negating
// -8000h (which has no positive s16 counterpart).
int32_t NegVol(int16_t v)
{
    return (v == -0x8000) ? 0x7FFF : -static_cast<int32_t>(v);
}

constexpr int16_t S16(uint16_t bits) { return static_cast<int16_t>(bits); }

// clang-format off
constexpr Preset kPresets[static_cast<int>(PresetId::Count)] = {
    // --- Off / size=80h bytes --------------------------------------------
    // NOTE: psx-spx explicitly calls for offset fields of 0001h (not 0000h)
    // "otherwise zerofilling the reverb buffer seems to fail". Reproduced
    // verbatim.
    // NOTE on workAreaBytes: psx-spx's own worked example labels this preset
    // "size=10h dummy bytes", but this repo's shipped PsyQ constant table
    // (src/bodyprog/libsd/smf_tables.h: sd_reverb_area_size[0] == 0x80)
    // disagrees with that documentation text. Since the size is only used to
    // compute mBASE (it has no effect on the register values above, which
    // are unaffected either way), the repo's own concrete, directly
    // verifiable shipped value (0x80) is used here in preference to
    // psx-spx's documentation-only example figure.
    { PresetId::Off, "Off", 0x80, {
        /*dAPF1,dAPF2*/ 0x0000,0x0000, /*vIIR*/ S16(0x0000),
        /*vCOMB1-4*/ S16(0x0000),S16(0x0000),S16(0x0000),S16(0x0000), /*vWALL*/ S16(0x0000),
        /*vAPF1,vAPF2*/ S16(0x0000),S16(0x0000),
        /*mLSAME,mRSAME*/ 0x0001,0x0001, /*mLCOMB1,mRCOMB1*/ 0x0001,0x0001, /*mLCOMB2,mRCOMB2*/ 0x0001,0x0001,
        /*dLSAME,dRSAME*/ 0x0000,0x0000, /*mLDIFF,mRDIFF*/ 0x0001,0x0001, /*mLCOMB3,mRCOMB3*/ 0x0001,0x0001, /*mLCOMB4,mRCOMB4*/ 0x0001,0x0001,
        /*dLDIFF,dRDIFF*/ 0x0000,0x0000, /*mLAPF1,mRAPF1*/ 0x0001,0x0001, /*mLAPF2,mRAPF2*/ 0x0001,0x0001,
        /*vLIN,vRIN*/ S16(0x0000),S16(0x0000)
    }},
    // --- Room / size=26C0h bytes ---------------------------------------
    // Double-verified: matches psx-spx "Room" example verbatim AND matches
    // this game's own shipped PsyQ constant table
    // (src/bodyprog/libsd/smf_tables.h: _spu_rev_param[18..33], packed as
    // 16 little-endian u32 = 32 u16, decoding to exactly these values) --
    // this is the preset Silent Hill uses (SPU_REV_MODE_ROOM == 1, and
    // sd_reverb_area_size[1] == 0x26C0 matches this size exactly).
    { PresetId::Room, "Room", 0x26C0, {
        0x007D,0x005B, S16(0x6D80),
        S16(0x54B8),S16(0xBED0),S16(0x0000),S16(0x0000), S16(0xBA80),
        S16(0x5800),S16(0x5300),
        0x04D6,0x0333, 0x03F0,0x0227, 0x0374,0x01EF,
        0x0334,0x01B5, 0x0000,0x0000, 0x0000,0x0000, 0x0000,0x0000,
        0x0000,0x0000, 0x01B4,0x0136, 0x00B8,0x005C,
        S16(0x8000),S16(0x8000)
    }},
    // --- Studio Small / size=1F40h bytes -------------------------------
    { PresetId::StudioSmall, "Studio Small", 0x1F40, {
        0x0033,0x0025, S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4410),S16(0xC0F0), S16(0x9C00),
        S16(0x5280),S16(0x4EC0),
        0x03E4,0x031B, 0x03A4,0x02AF, 0x0372,0x0266,
        0x031C,0x025D, 0x025C,0x018E, 0x022F,0x0135, 0x01D2,0x00B7,
        0x018F,0x00B5, 0x00B4,0x0080, 0x004C,0x0026,
        S16(0x8000),S16(0x8000)
    }},
    // --- Studio Medium / size=4840h bytes ------------------------------
    { PresetId::StudioMedium, "Studio Medium", 0x4840, {
        0x00B1,0x007F, S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0), S16(0xB4C0),
        S16(0x5280),S16(0x4EC0),
        0x0904,0x076B, 0x0824,0x065F, 0x07A2,0x0616,
        0x076C,0x05ED, 0x05EC,0x042E, 0x050F,0x0305, 0x0462,0x02B7,
        0x042F,0x0265, 0x0264,0x01B2, 0x0100,0x0080,
        S16(0x8000),S16(0x8000)
    }},
    // --- Studio Large / size=6FE0h bytes -------------------------------
    { PresetId::StudioLarge, "Studio Large", 0x6FE0, {
        0x00E3,0x00A9, S16(0x6F60),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0), S16(0xA680),
        S16(0x5680),S16(0x52C0),
        0x0DFB,0x0B58, 0x0D09,0x0A3C, 0x0BD9,0x0973,
        0x0B59,0x08DA, 0x08D9,0x05E9, 0x07EC,0x04B0, 0x06EF,0x03D2,
        0x05EA,0x031D, 0x031C,0x0238, 0x0154,0x00AA,
        S16(0x8000),S16(0x8000)
    }},
    // --- Hall / size=ADE0h bytes ----------------------------------------
    { PresetId::Hall, "Hall", 0xADE0, {
        0x01A5,0x0139, S16(0x6000),
        S16(0x5000),S16(0x4C00),S16(0xB800),S16(0xBC00), S16(0xC000),
        S16(0x6000),S16(0x5C00),
        0x15BA,0x11BB, 0x14C2,0x10BD, 0x11BC,0x0DC1,
        0x11C0,0x0DC3, 0x0DC0,0x09C1, 0x0BC4,0x07C1, 0x0A00,0x06CD,
        0x09C2,0x05C1, 0x05C0,0x041A, 0x0274,0x013A,
        S16(0x8000),S16(0x8000)
    }},
    // --- Space Echo / size=F6C0h bytes ----------------------------------
    { PresetId::SpaceEcho, "Space Echo", 0xF6C0, {
        0x033D,0x0231, S16(0x7E00),
        S16(0x5000),S16(0xB400),S16(0xB000),S16(0x4C00), S16(0xB000),
        S16(0x6000),S16(0x5400),
        0x1ED6,0x1A31, 0x1D14,0x183B, 0x1BC2,0x16B2,
        0x1A32,0x15EF, 0x15EE,0x1055, 0x1334,0x0F2D, 0x11F6,0x0C5D,
        0x1056,0x0AE1, 0x0AE0,0x07A2, 0x0464,0x0232,
        S16(0x8000),S16(0x8000)
    }},
    // --- Chaos Echo (almost infinite) / size=18040h bytes ---------------
    { PresetId::ChaosEcho, "Chaos Echo", 0x18040, {
        0x0001,0x0001, S16(0x7FFF),
        S16(0x7FFF),S16(0x0000),S16(0x0000),S16(0x0000), S16(0x8100),
        S16(0x0000),S16(0x0000),
        0x1FFF,0x0FFF, 0x1005,0x0005, 0x0000,0x0000,
        0x1005,0x0005, 0x0000,0x0000, 0x0000,0x0000, 0x0000,0x0000,
        0x0000,0x0000, 0x1004,0x1002, 0x0004,0x0002,
        S16(0x8000),S16(0x8000)
    }},
    // --- Delay (one-shot echo) / size=18040h bytes -----------------------
    { PresetId::Delay, "Delay", 0x18040, {
        0x0001,0x0001, S16(0x7FFF),
        S16(0x7FFF),S16(0x0000),S16(0x0000),S16(0x0000), S16(0x0000),
        S16(0x0000),S16(0x0000),
        0x1FFF,0x0FFF, 0x1005,0x0005, 0x0000,0x0000,
        0x1005,0x0005, 0x0000,0x0000, 0x0000,0x0000, 0x0000,0x0000,
        0x0000,0x0000, 0x1004,0x1002, 0x0004,0x0002,
        S16(0x8000),S16(0x8000)
    }},
    // --- Half Echo / size=3C00h bytes -----------------------------------
    { PresetId::HalfEcho, "Half Echo", 0x3C00, {
        0x0017,0x0013, S16(0x70F0),
        S16(0x4FA8),S16(0xBCE0),S16(0x4510),S16(0xBEF0), S16(0x8500),
        S16(0x5F80),S16(0x54C0),
        0x0371,0x02AF, 0x02E5,0x01DF, 0x02B0,0x01D7,
        0x0358,0x026A, 0x01D6,0x011E, 0x012D,0x00B1, 0x011F,0x0059,
        0x01A0,0x00E3, 0x0058,0x0040, 0x0028,0x0014,
        S16(0x8000),S16(0x8000)
    }},
};
// clang-format on

} // namespace

const Preset& GetPreset(PresetId id)
{
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= static_cast<int>(PresetId::Count))
        idx = static_cast<int>(PresetId::Off);
    return kPresets[idx];
}

Reverb::Reverb()
{
    Reset();
}

void Reverb::Reset()
{
    m_regs = Registers{};
    m_mBaseRaw = 0;
    m_baseAddress = 0;
    m_currentAddress = 0;
    m_vLOUT = 0;
    m_vROUT = 0;
    m_masterEnable = false;
    std::memset(m_downsampleBuffer, 0, sizeof(m_downsampleBuffer));
    std::memset(m_upsampleBuffer, 0, sizeof(m_upsampleBuffer));
    m_resamplePos = 0;
}

void Reverb::ClearDelayState()
{
    m_currentAddress = m_baseAddress;
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
    SetRegisters(preset.regs);
}

void Reverb::SetRegisterRaw(unsigned index, uint16_t value)
{
    if (index >= 32u)
        return;
    // Registers has no padding (guarded by the static_assert in the header)
    // and every field is exactly 16 bits, so it can be addressed as a flat
    // u16[32] array -- this mirrors DuckStation's own
    // `union { struct {...}; u16 rev[32]; }` idiom. memcpy (rather than a
    // reinterpret_cast<uint16_t*>) is used so this stays well-defined under
    // the strict-aliasing rule.
    std::memcpy(reinterpret_cast<uint8_t*>(&m_regs) + index * sizeof(uint16_t), &value, sizeof(value));
}

uint16_t Reverb::GetRegisterRaw(unsigned index) const
{
    if (index >= 32u)
        return 0;
    uint16_t value;
    std::memcpy(&value, reinterpret_cast<const uint8_t*>(&m_regs) + index * sizeof(uint16_t), sizeof(value));
    return value;
}

void Reverb::SetBaseAddress(uint16_t mBaseRaw)
{
    // psx-spx: "Writing a value to mBASE does additionally set the current
    // buffer address to that value." / DuckStation spu.cpp:990-992.
    m_mBaseRaw = mBaseRaw;
    m_baseAddress = (static_cast<uint32_t>(mBaseRaw) << 2) & 0x3FFFFu;
    m_currentAddress = m_baseAddress;
}

uint32_t Reverb::MemoryHalfwordIndex(uint32_t currentAddress, uint32_t baseAddress,
                                     uint32_t rawHalfwordOffset)
{
    // Ported from DuckStation SPU::ReverbMemoryAddress (spu.cpp:2194-2203),
    // itself operating on a fixed 18-bit halfword address space (512 KiB
    // bytes / 2). `currentAddress` and `baseAddress` are always already
    // masked to this range by SetBaseAddress()/Process(), so the sum below
    // can only ever occupy 19 bits (max 0x3FFFF+0x3FFFF = 0x7FFFE).
    constexpr uint32_t kMask = 0x3FFFFu; // (kSpuRamBytes/2) - 1

    uint32_t offset = currentAddress + (rawHalfwordOffset & kMask);
    // DuckStation computes this via a branchless sign-shift trick
    // (`offset += base & ((s32)(offset<<13)>>31)`), which is exactly
    // equivalent to "if this sum spilled past the 18-bit halfword space,
    // rebase it relative to baseAddress instead of relative to absolute 0"
    // given the bounded 19-bit range above; written explicitly here for
    // scalar clarity.
    if (offset & 0x40000u)
        offset += baseAddress;
    return offset & kMask;
}

int16_t Reverb::ReverbRead(const uint8_t* ram, uint32_t currentAddress, uint32_t baseAddress,
                            uint32_t rawFieldValue, int32_t extraHalfwords)
{
    // Register values are addresses "divided by 8" (psx-spx); <<2 converts
    // an 8-byte unit into a 2-byte (halfword) unit (8/2 = 4 = 1<<2).
    const uint32_t rawOffset = (rawFieldValue << 2) + static_cast<uint32_t>(extraHalfwords);
    const uint32_t idx = MemoryHalfwordIndex(currentAddress, baseAddress, rawOffset);
    const uint32_t byteAddr = idx * 2u;
    int16_t v;
    std::memcpy(&v, ram + byteAddr, sizeof(v));
    return v;
}

void Reverb::ReverbWrite(uint8_t* ram, uint32_t currentAddress, uint32_t baseAddress,
                         uint32_t rawFieldValue, int16_t value)
{
    const uint32_t rawOffset = rawFieldValue << 2;
    const uint32_t idx = MemoryHalfwordIndex(currentAddress, baseAddress, rawOffset);
    const uint32_t byteAddr = idx * 2u;
    std::memcpy(ram + byteAddr, &value, sizeof(value));
}

void Reverb::Process(uint8_t* ram, uint32_t ramSizeBytes,
                      int32_t inputLeft, int32_t inputRight,
                      int32_t* outLeft, int32_t* outRight)
{
    // The wrap math is always relative to the fixed real hardware size; the
    // parameter is only sanity-checked (a too-small buffer is a caller bug).
    (void)ramSizeBytes;

    const Registers& r = m_regs;
    const uint32_t curAddr = m_currentAddress;
    const uint32_t baseAddr = m_baseAddress;

    // --- Step 1: feed the 44.1kHz downsample history (always happens). ---
    const int16_t inL16 = Truncate16(inputLeft);
    const int16_t inR16 = Truncate16(inputRight);
    m_downsampleBuffer[0][m_resamplePos | 0x00u] = inL16;
    m_downsampleBuffer[0][m_resamplePos | 0x40u] = inL16;
    m_downsampleBuffer[1][m_resamplePos | 0x00u] = inR16;
    m_downsampleBuffer[1][m_resamplePos | 0x40u] = inR16;

    int32_t out[2];

    if (m_resamplePos & 1u)
    {
        // --- Step 2: downsample 44.1kHz -> 22.05kHz (odd ticks only). ----
        // 39-tap half-band FIR: sum the 20 nonzero even-index taps (stride 2
        // into the 39-sample window) plus the separate centre tap (index 19).
        int32_t downsampled[2];
        for (int ch = 0; ch < 2; ch++)
        {
            const int16_t* src = &m_downsampleBuffer[ch][(m_resamplePos - 38u) & 0x3Fu];
            int32_t acc = 0;
            for (int i = 0; i < 20; i++)
                acc += kFirCoeffs[i] * static_cast<int32_t>(src[2 * i]);
            acc += kFirCenterTap * static_cast<int32_t>(src[19]);
            downsampled[ch] = Clamp16(acc >> 15);
        }

        // --- Step 3: the core reverb recursion (runs once per 22.05kHz tick, i.e. per odd 44.1kHz tick). ---
        const uint16_t mSAME[2] = { r.mLSAME, r.mRSAME };
        const uint16_t dSAME[2] = { r.dLSAME, r.dRSAME };
        const uint16_t mDIFF[2] = { r.mLDIFF, r.mRDIFF };
        const uint16_t dDIFF[2] = { r.dLDIFF, r.dRDIFF };
        const uint16_t mCOMB1[2] = { r.mLCOMB1, r.mRCOMB1 };
        const uint16_t mCOMB2[2] = { r.mLCOMB2, r.mRCOMB2 };
        const uint16_t mCOMB3[2] = { r.mLCOMB3, r.mRCOMB3 };
        const uint16_t mCOMB4[2] = { r.mLCOMB4, r.mRCOMB4 };
        const uint16_t mAPF1[2] = { r.mLAPF1, r.mRAPF1 };
        const uint16_t mAPF2[2] = { r.mLAPF2, r.mRAPF2 };
        const int16_t inCoef[2] = { r.vLIN, r.vRIN };

        for (int ch = 0; ch < 2; ch++)
        {
            const int other = ch ^ 1;

            if (m_masterEnable)
            {
                // Input from mixer: same-side uses this channel's own delay
                // (dSAME[ch]), different-side (cross) uses the OTHER
                // channel's dDIFF (psx-spx: "[mLDIFF]=(Lin+[dRDIFF]*vWALL...")
                const int32_t iirInputSame = Clamp16(
                    (((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, dSAME[ch], 0)) *
                       static_cast<int32_t>(r.vWALL)) >> 14) +
                     ((downsampled[ch] * static_cast<int32_t>(inCoef[ch])) >> 14)) >> 1);
                const int32_t iirInputDiff = Clamp16(
                    (((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, dDIFF[other], 0)) *
                       static_cast<int32_t>(r.vWALL)) >> 14) +
                     ((downsampled[ch] * static_cast<int32_t>(inCoef[ch])) >> 14)) >> 1);

                // Same/Different Side Reflection recursive IIR, including the
                // vIIR==-8000h negation quirk (psx-spx "Bug").
                const int16_t oldSame = ReverbRead(ram, curAddr, baseAddr, mSAME[ch], -1);
                const int16_t oldDiff = ReverbRead(ram, curAddr, baseAddr, mDIFF[ch], -1);
                const int32_t iirSame = Clamp16(
                    (((iirInputSame * static_cast<int32_t>(r.vIIR)) >> 14) +
                     (ReflectFeedback(oldSame, r.vIIR) >> 14)) >> 1);
                const int32_t iirDiff = Clamp16(
                    (((iirInputDiff * static_cast<int32_t>(r.vIIR)) >> 14) +
                     (ReflectFeedback(oldDiff, r.vIIR) >> 14)) >> 1);

                ReverbWrite(ram, curAddr, baseAddr, mSAME[ch], Truncate16(iirSame));
                ReverbWrite(ram, curAddr, baseAddr, mDIFF[ch], Truncate16(iirDiff));
            }

            // Early Echo (Comb Filter). Reads still occur even when reverb
            // is master-disabled (psx-spx "Reverb Disable").
            const int32_t acc =
                ((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, mCOMB1[ch], 0)) * static_cast<int32_t>(r.vCOMB1)) >> 14) +
                ((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, mCOMB2[ch], 0)) * static_cast<int32_t>(r.vCOMB2)) >> 14) +
                ((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, mCOMB3[ch], 0)) * static_cast<int32_t>(r.vCOMB3)) >> 14) +
                ((static_cast<int32_t>(ReverbRead(ram, curAddr, baseAddr, mCOMB4[ch], 0)) * static_cast<int32_t>(r.vCOMB4)) >> 14);

            // Late Reverb APF1/APF2. Address subtraction (mAPFn - dAPFn) is
            // done in raw, wraparound register-address space BEFORE the <<2
            // unit conversion, exactly matching hardware/DuckStation
            // (`MIX_DEST_A[channel] - FB_SRC_A`); doing the subtraction in
            // uint32_t here (rather than uint16_t) is equivalent because the
            // result is always re-masked to 18 bits (a multiple of 2^16)
            // before use.
            const uint32_t apf1Addr = static_cast<uint32_t>(mAPF1[ch]) - static_cast<uint32_t>(r.dAPF1);
            const uint32_t apf2Addr = static_cast<uint32_t>(mAPF2[ch]) - static_cast<uint32_t>(r.dAPF2);
            const int32_t fbA = ReverbRead(ram, curAddr, baseAddr, apf1Addr, 0);
            const int32_t fbB = ReverbRead(ram, curAddr, baseAddr, apf2Addr, 0);

            const int32_t mda = Clamp16((acc + ((fbA * NegVol(r.vAPF1)) >> 14)) >> 1);
            const int32_t mdb = Clamp16(fbA + ((((mda * static_cast<int32_t>(r.vAPF1)) >> 14) +
                                                 ((fbB * NegVol(r.vAPF2)) >> 14)) >> 1));

            // 22.05kHz-rate reverb output sample for this channel, stored
            // into the upsample history (mirrored at +0x20, same trick as
            // the downsample buffer's +0x40 mirror).
            const int16_t newUp = Truncate16(Clamp16(fbB + ((mdb * static_cast<int32_t>(r.vAPF2)) >> 15)));
            m_upsampleBuffer[ch][(m_resamplePos >> 1) | 0x20u] = newUp;
            m_upsampleBuffer[ch][m_resamplePos >> 1] = newUp;

            if (m_masterEnable)
            {
                ReverbWrite(ram, curAddr, baseAddr, mAPF1[ch], Truncate16(mda));
                ReverbWrite(ram, curAddr, baseAddr, mAPF2[ch], Truncate16(mdb));
            }
        }

        // --- Step 4: advance the work-area address, once per 22.05kHz tick. ---
        // psx-spx: "BufferAddress = MAX(mBASE, (BufferAddress+2) AND 7FFFEh)".
        // Equivalent formulation (see header comment / DuckStation
        // spu.cpp:2337-2339): current address only ever increases by 1
        // halfword per tick starting from base, so it can only ever "fall
        // below base" by wrapping exactly to 0, at which point resetting to
        // base is identical to MAX(base, wrapped).
        m_currentAddress = (m_currentAddress + 1u) & 0x3FFFFu;
        if (m_currentAddress == 0u)
            m_currentAddress = m_baseAddress;

        // --- Step 5: upsample 22.05kHz -> 44.1kHz output for THIS (odd) tick. ---
        for (int ch = 0; ch < 2; ch++)
        {
            const int16_t* src = &m_upsampleBuffer[ch][((m_resamplePos >> 1) - 19u) & 0x1Fu];
            int32_t acc = 0;
            for (int i = 0; i < 20; i++)
                acc += kFirCoeffs[i] * static_cast<int32_t>(src[i]);
            out[ch] = Clamp16(acc >> 14);
        }
    }
    else
    {
        // Even ticks: pass the already-upsampled raw sample straight through
        // (mathematically the "zero-stuffed" interpolation position, which
        // this specific half-band filter design reproduces exactly without
        // needing any further filtering -- see DuckStation spu.cpp:2358-2363).
        const size_t idx = (((m_resamplePos >> 1) - 19u) & 0x1Fu) + 9u;
        for (int ch = 0; ch < 2; ch++)
            out[ch] = m_upsampleBuffer[ch][idx];
    }

    m_resamplePos = (m_resamplePos + 1u) & 0x3Fu;

    *outLeft = ApplyVolume(out[0], m_vLOUT);
    *outRight = ApplyVolume(out[1], m_vROUT);
}

} // namespace PsyX_SPUReverb

#ifdef PSYX_SPUREVERB_STANDALONE_TEST

// Deterministic, dependency-free self-test.
// Build & run standalone with, e.g.:
//   g++ -std=c++17 -DPSYX_SPUREVERB_STANDALONE_TEST=1 -Wall -Wextra
//       PsyX_SPUReverb.cpp -o test_reverb && ./test_reverb
//
// This macro is never defined by the normal library build (CMakeLists.txt
// globs *.cpp unconditionally with no such define), so this block compiles
// out of the shipped library entirely.

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace PsyX_SPUReverb;

int g_failures = 0;

void Check(bool cond, const char* what)
{
    if (!cond)
    {
        std::fprintf(stderr, "[FAIL] %s\n", what);
        g_failures++;
    }
    else
    {
        std::printf("[ OK ] %s\n", what);
    }
}

// Test 1: pure silence in -> pure silence out, forever, regardless of preset.
void TestSilence()
{
    std::vector<uint8_t> ram(kSpuRamBytes, 0);
    Reverb rv;
    rv.LoadPreset(PresetId::Room);
    rv.SetBaseAddress(static_cast<uint16_t>((kSpuRamBytes - GetPreset(PresetId::Room).workAreaBytes) / 8));
    rv.SetOutputVolume(static_cast<int16_t>(0x7FFF), static_cast<int16_t>(0x7FFF));
    rv.SetMasterEnable(true);

    bool allSilent = true;
    for (int i = 0; i < 20000; i++)
    {
        int32_t l, r;
        rv.Process(ram.data(), static_cast<uint32_t>(ram.size()), 0, 0, &l, &r);
        if (l != 0 || r != 0)
        {
            allSilent = false;
            break;
        }
    }
    Check(allSilent, "Silence: zero input over 20000 ticks produces exactly zero output");
}

// Test 2: an impulse eventually produces nonzero (nonsilent) output, and all
// outputs stay in valid s16 range.
void TestImpulse()
{
    std::vector<uint8_t> ram(kSpuRamBytes, 0);
    Reverb rv;
    rv.LoadPreset(PresetId::Room);
    rv.SetBaseAddress(static_cast<uint16_t>((kSpuRamBytes - GetPreset(PresetId::Room).workAreaBytes) / 8));
    rv.SetOutputVolume(static_cast<int16_t>(0x7FFF), static_cast<int16_t>(0x7FFF));
    rv.SetMasterEnable(true);

    bool sawNonzero = false;
    bool inRange = true;
    for (int i = 0; i < 60000; i++)
    {
        const int32_t inL = (i == 0) ? 0x4000 : 0;
        const int32_t inR = (i == 0) ? 0x4000 : 0;
        int32_t l, r;
        rv.Process(ram.data(), static_cast<uint32_t>(ram.size()), inL, inR, &l, &r);
        if (l != 0 || r != 0)
            sawNonzero = true;
        if (l < -32768 || l > 32767 || r < -32768 || r > 32767)
            inRange = false;
    }
    Check(sawNonzero, "Impulse: single impulse produces nonzero output within 60000 ticks");
    Check(inRange, "Impulse: all outputs stay within signed 16-bit range");
}

// Test 3: work-area address wrap never touches RAM outside [base, 0x7FFFE].
void TestAddressWrap()
{
    std::vector<uint8_t> ram(kSpuRamBytes);
    const uint8_t sentinel = 0xCD;
    std::memset(ram.data(), sentinel, ram.size());

    // Force a tiny work area near the very end of RAM so many wraps happen
    // quickly and any out-of-area write is easy to detect.
    //
    // IMPORTANT: every address-register offset below must resolve to a
    // halfword offset strictly smaller than the work area itself. On real
    // hardware, the wrap logic only rebases relative to mBASE once the
    // current-position-plus-offset sum spills past the fixed top of the
    // 18-bit halfword space (0x3FFFF) -- it is *not* clamped to the work
    // area's own size. That means an offset field bigger than the work area
    // (e.g. naively reusing PresetId::Room's offsets, which assume its own
    // documented 0x26C0-byte area, together with an artificially tiny mBASE)
    // is a physically-invalid configuration that a real game/PsyQ never
    // produces (presets always ship with a "size=" big enough to hold their
    // own offsets) and can legitimately land outside [mBASE, 0x7FFFE] here.
    // So this test uses its own small, self-consistent preset instead: an
    // 0x40-byte (32-halfword) work area with every offset kept well under
    // that, while still exercising every stage of the recursion (nonzero
    // vIIR/vCOMB/vWALL/vAPF and the mSAME/mDIFF -2-byte lookback).
    Preset tiny{};
    tiny.id = PresetId::Count; // not a real preset id, just a local scratch value
    tiny.name = "TestTiny";
    tiny.workAreaBytes = 0x40; // 32 halfwords
    Registers& tr = tiny.regs;
    tr.dAPF1 = 1; tr.dAPF2 = 1;
    tr.vIIR = static_cast<int16_t>(0x4000);
    tr.vCOMB1 = static_cast<int16_t>(0x2000); tr.vCOMB2 = static_cast<int16_t>(0x2000);
    tr.vCOMB3 = static_cast<int16_t>(0x1000); tr.vCOMB4 = static_cast<int16_t>(0x1000);
    tr.vWALL = static_cast<int16_t>(0x4000);
    tr.vAPF1 = static_cast<int16_t>(0x4000); tr.vAPF2 = static_cast<int16_t>(0x4000);
    tr.mLSAME = 3; tr.mRSAME = 3;
    tr.mLCOMB1 = 2; tr.mRCOMB1 = 2;
    tr.mLCOMB2 = 3; tr.mRCOMB2 = 3;
    tr.dLSAME = 1; tr.dRSAME = 1;
    tr.mLDIFF = 3; tr.mRDIFF = 3;
    tr.mLCOMB3 = 4; tr.mRCOMB3 = 4;
    tr.mLCOMB4 = 5; tr.mRCOMB4 = 5;
    tr.dLDIFF = 1; tr.dRDIFF = 1;
    tr.mLAPF1 = 6; tr.mRAPF1 = 6;
    tr.mLAPF2 = 7; tr.mRAPF2 = 7;
    tr.vLIN = static_cast<int16_t>(0x4000); tr.vRIN = static_cast<int16_t>(0x4000);

    Reverb rv;
    rv.LoadPreset(tiny);
    const uint16_t mBaseRaw = static_cast<uint16_t>((kSpuRamBytes - tiny.workAreaBytes) / 8u);
    rv.SetBaseAddress(mBaseRaw);
    rv.SetOutputVolume(static_cast<int16_t>(0x7FFF), static_cast<int16_t>(0x7FFF));
    rv.SetMasterEnable(true);

    const uint32_t baseByteAddr = (static_cast<uint32_t>(mBaseRaw) << 2) * 2u; // raw(<<2 halfwords)*2=bytes
    // Drive plenty of ticks to force many address wraps.
    for (int i = 0; i < 200000; i++)
    {
        const int32_t inL = ((i % 977) == 0) ? 0x1234 : 0;
        const int32_t inR = ((i % 613) == 0) ? -0x4321 : 0;
        int32_t l, r;
        rv.Process(ram.data(), static_cast<uint32_t>(ram.size()), inL, inR, &l, &r);
        (void)l; (void)r;
    }

    bool untouchedOutside = true;
    for (uint32_t addr = 0; addr < baseByteAddr; addr++)
    {
        if (ram[addr] != sentinel)
        {
            untouchedOutside = false;
            break;
        }
    }
    if (kWorkAreaEndAddress + 2u < ram.size())
    {
        for (uint32_t addr = kWorkAreaEndAddress + 2u; addr < ram.size(); addr++)
        {
            if (ram[addr] != sentinel)
            {
                untouchedOutside = false;
                break;
            }
        }
    }
    Check(untouchedOutside,
          "Address wrap: 200000 ticks with a tiny end-of-RAM work area never write outside [mBASE, 0x7FFFE]");
}

} // namespace

int main()
{
    TestSilence();
    TestImpulse();
    TestAddressWrap();

    if (g_failures == 0)
    {
        std::printf("\nAll PsyX_SPUReverb self-tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d PsyX_SPUReverb self-test(s) FAILED.\n", g_failures);
    return 1;
}

#endif // PSYX_SPUREVERB_STANDALONE_TEST
