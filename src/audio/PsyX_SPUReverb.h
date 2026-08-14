#ifndef PSYX_SPUREVERB_H
#define PSYX_SPUREVERB_H

// Backend-neutral, standalone PS1 SPU reverb DSP core.
//
// Implements ONLY the reverb signal-processing block documented at psx-spx
// ( https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-reverb-registers ,
// "SPU Reverb Formula" / "SPU Reverb Examples" / "Reverb Buffer Resampling" /
// "Reverb Computation Order" sections ), cross-verified line-by-line against
// DuckStation's SPU::ProcessReverb / ReverbRead / ReverbWrite /
// ReverbMemoryAddress (github.com/stenzek/duckstation, src/core/spu.cpp,
// itself explicitly credited there to "Mednafen-PSX, rewritten/vectorized").
// This module owns no I/O ports, no voice mixing, no ADSR/ADPCM/EON logic and
// no SPUCNT bits beyond the single reverb-master-enable flag: the caller
// (SPU core) is responsible for summing the per-voice/CD/external samples
// that have their reverb-send bit set and handing the per-tick sum to
// Process().
//
// RAM ownership: the real PS1 SPU has ONE shared 512 KiB RAM; the reverb work
// area lives at the *end* of that RAM (fixed end address 0x7FFFE, in bytes)
// starting at a programmable base address (mBASE). This module never
// allocates or owns RAM -- callers pass a pointer to the live, shared 512 KiB
// SPU RAM buffer on every Process() call, exactly like hardware, so voice
// writes and reverb reads/writes correctly alias through the same memory.
//
// Arithmetic assumptions (matching every known real-world PS1 emulator,
// including DuckStation/Mednafen-PSX, and all mainstream compilers):
//   - int32_t is 2's complement, `>>` on a negative int32_t is an arithmetic
//     (sign-extending) shift, and signed overflow silently wraps mod 2^32.
//     (Technically UB / impl-defined pre-C++20, but universally implemented
//     this way by GCC/Clang/MSVC; DuckStation relies on the same assumption.)

#include <cstdint>
#include <cstddef>

namespace PsyX_SPUReverb {

// Real (non-mirrored) PS1 SPU RAM size. Process() assumes `ram` points at a
// buffer of at least this size; the reverb work-area wrap logic is hardwired
// to this size exactly as on real hardware (mBASE only moves the *start* of
// the work area -- the *end* is permanently fixed at kWorkAreaEndAddress).
constexpr uint32_t kSpuRamBytes = 512u * 1024u;

// Fixed end address of the reverb work area (1 halfword before the very end
// of the 512 KiB RAM), in bytes. See psx-spx: "the end address is fixed, at
// 7FFFEh".
constexpr uint32_t kWorkAreaEndAddress = 0x7FFFEu;

// One-to-one with the 32 x 16-bit hardware registers at 1F801DC0h-1F801DFEh,
// in port order (index 0 = 1F801DC0h/rev00 ... index 31 = 1F801DFEh/rev1F).
// Field names match psx-spx's mnemonics exactly, and this exact field order
// was verified against psx-spx's "SPU Reverb Registers" port table (rev00
// dAPF1 ... rev1F vRIN). "src/dst/disp/base" registers are unsigned (they
// are raw bit patterns used only for address arithmetic -- see psx-spx: "All
// src/dst/disp/base registers are addresses in SPU memory (divided by 8)");
// "volume" registers are signed (they are used as fixed-point multiplier
// coefficients).
struct Registers
{
    uint16_t dAPF1, dAPF2;                   // rev00,01 - APF offsets 1/2
    int16_t  vIIR;                           // rev02    - reflection volume 1
    int16_t  vCOMB1, vCOMB2, vCOMB3, vCOMB4; // rev03-06 - comb volumes 1-4
    int16_t  vWALL;                          // rev07    - reflection volume 2
    int16_t  vAPF1, vAPF2;                   // rev08,09 - APF volumes 1/2
    uint16_t mLSAME, mRSAME;                 // rev0A,0B - same-side reflect addr 1 L/R
    uint16_t mLCOMB1, mRCOMB1;                // rev0C,0D - comb addr 1 L/R
    uint16_t mLCOMB2, mRCOMB2;                // rev0E,0F - comb addr 2 L/R
    uint16_t dLSAME, dRSAME;                 // rev10,11 - same-side reflect addr 2 L/R
    uint16_t mLDIFF, mRDIFF;                 // rev12,13 - diff-side reflect addr 1 L/R
    uint16_t mLCOMB3, mRCOMB3;                // rev14,15 - comb addr 3 L/R
    uint16_t mLCOMB4, mRCOMB4;                // rev16,17 - comb addr 4 L/R
    uint16_t dLDIFF, dRDIFF;                 // rev18,19 - diff-side reflect addr 2 L/R
    uint16_t mLAPF1, mRAPF1;                  // rev1A,1B - APF addr 1 L/R
    uint16_t mLAPF2, mRAPF2;                  // rev1C,1D - APF addr 2 L/R
    int16_t  vLIN, vRIN;                     // rev1E,1F - input volume L/R
};

static_assert(sizeof(Registers) == 32u * sizeof(uint16_t),
              "Registers must be exactly 32 packed 16-bit fields (no padding) "
              "to match the real 1F801DC0h-1F801DFEh port layout 1:1.");

// PsyQ SPU_REV_MODE_* names (include/psyq/libspu.h). Byte-size ordering
// verified against this repo's shipped libsd constant tables
// (src/bodyprog/libsd/smf_tables.h: sd_reverb_area_size[10]): 9 of the 10
// entries (Room..HalfEcho) match psx-spx's published example sizes exactly,
// giving a verified PresetId <-> psx-spx-example-name correspondence. The
// one exception is "Off": psx-spx documents a "size=10h dummy bytes" example,
// but this game's own sd_reverb_area_size[0] is 0x80 -- see the comment on
// the Off preset entry in the .cpp for details; the repo's own shipped value
// is used there since it's the more concrete, directly-verifiable source.
enum class PresetId : int
{
    Off = 0,          // PsyQ OFF           / psx-spx "Reverb off"
    Room,             // PsyQ ROOM          / psx-spx "Room"           <- used by Silent Hill
    StudioSmall,      // PsyQ STUDIO_A      / psx-spx "Studio Small"
    StudioMedium,     // PsyQ STUDIO_B      / psx-spx "Studio Medium"
    StudioLarge,      // PsyQ STUDIO_C      / psx-spx "Studio Large"
    Hall,             // PsyQ HALL          / psx-spx "Hall"
    SpaceEcho,        // PsyQ SPACE         / psx-spx "Space Echo"
    ChaosEcho,        // PsyQ ECHO          / psx-spx "Chaos Echo (almost infinite)"
    Delay,            // PsyQ DELAY         / psx-spx "Delay (one-shot echo)"
    HalfEcho,         // PsyQ PIPE          / psx-spx "Half Echo"
    Count
};

struct Preset
{
    PresetId id;
    const char* name;
    uint32_t workAreaBytes; // matches psx-spx "size=" / PsyQ sd_reverb_area_size[]
    Registers regs;
};

// Returns the built-in preset table entry for `id`. All entries are taken
// verbatim from psx-spx's published register dumps ("SPU Reverb Examples").
// PresetId::Room is additionally cross-checked byte-for-byte against this
// game's own shipped PsyQ `_spu_rev_param` constant table (see
// PsyX_SPUReverb.cpp for the comparison) -- it is the only preset with two
// independent sources; the rest are transcribed from psx-spx alone.
const Preset& GetPreset(PresetId id);

// Reverb DSP core. One instance == one PS1 SPU's reverb unit. Stateless with
// respect to RAM: the caller supplies the live, shared 512 KiB SPU RAM buffer
// on every Process() call.
class Reverb
{
public:
    Reverb();

    // Resets registers to all-zero, base/current address to zero, master
    // enable to false, and clears the internal FIR resample history. Does
    // NOT touch caller-owned SPU RAM.
    void Reset();
    void ClearDelayState();

    void LoadPreset(PresetId id);
    void LoadPreset(const Preset& preset);

    // Programs all 32 reverb registers at once. Does not touch mBASE/current
    // address (mBASE lives at 1F801DA2h, outside the 1F801DC0h register
    // block, and is programmed separately via SetBaseAddress()).
    void SetRegisters(const Registers& regs) { m_regs = regs; }
    const Registers& GetRegisters() const { return m_regs; }

    // Raw single-register access matching hardware port order (index 0 =
    // 1F801DC0h/rev00 ... index 31 = 1F801DFEh/rev1F), for callers that want
    // to forward raw 16-bit port writes without needing to know field
    // semantics.
    void SetRegisterRaw(unsigned index, uint16_t value);
    uint16_t GetRegisterRaw(unsigned index) const;

    // 1F801DA2h (mBASE), raw port encoding (units of 8 bytes). Writing it
    // resets the current work-area address to the base address, exactly
    // like hardware ("Writing a value to mBASE does additionally set the
    // current buffer address to that value.").
    void SetBaseAddress(uint16_t mBaseRaw);
    uint16_t GetBaseAddressRaw() const { return m_mBaseRaw; }

    // 1F801D84h/1F801D86h - reverb output volume, applied after APF2.
    void SetOutputVolume(int16_t vLOUT, int16_t vROUT) { m_vLOUT = vLOUT; m_vROUT = vROUT; }

    // SPUCNT.bit7 (Reverb Master Enable). When false: buffer *writes* stop
    // (SAME/DIFF stages and the APF1/APF2 write-back are skipped), matching
    // psx-spx's documented "Reverb Disable" semantics; reads/recirculation
    // through COMB/APF continue using whatever is already in the buffer.
    void SetMasterEnable(bool enabled) { m_masterEnable = enabled; }
    bool GetMasterEnable() const { return m_masterEnable; }

    // Runs one 44.1 kHz sample tick. `ram` must point at the live, shared
    // SPU RAM (mutable: this call both reads and writes it) and must be at
    // least kSpuRamBytes long (ramSizeBytes is only asserted against, the
    // wrap math is always relative to the fixed kSpuRamBytes/kWorkAreaEndAddress
    // regardless of the value passed). `inputLeft`/`inputRight` are the
    // pre-vLIN/vRIN mixer sums (sum of every voice/CD/external sample that
    // has its reverb-send bit set), NOT pre-clamped by the caller -- matching
    // hardware/DuckStation, this core truncates (wraps, does NOT saturate)
    // them to 16 bits as its very first step.
    void Process(uint8_t* ram, uint32_t ramSizeBytes,
                 int32_t inputLeft, int32_t inputRight,
                 int32_t* outLeft, int32_t* outRight);

private:
    static uint32_t MemoryHalfwordIndex(uint32_t currentAddress, uint32_t baseAddress,
                                         uint32_t rawHalfwordOffset);
    static int16_t ReverbRead(const uint8_t* ram, uint32_t currentAddress, uint32_t baseAddress,
                               uint32_t rawFieldValue, int32_t extraHalfwords);
    static void ReverbWrite(uint8_t* ram, uint32_t currentAddress, uint32_t baseAddress,
                            uint32_t rawFieldValue, int16_t value);

    Registers m_regs{};
    uint16_t m_mBaseRaw = 0;
    uint32_t m_baseAddress = 0;    // halfword units, 18-bit (0..0x3FFFF)
    uint32_t m_currentAddress = 0; // halfword units, 18-bit (0..0x3FFFF)
    int16_t m_vLOUT = 0;
    int16_t m_vROUT = 0;
    bool m_masterEnable = false;

    // 44.1 kHz-rate downsample history, duplicated at +64 so a 39-tap window
    // never needs an explicit modulo per sample (see .cpp for the mirroring
    // trick, ported from DuckStation).
    int16_t m_downsampleBuffer[2][128]{};
    // 22.05 kHz-rate upsample history, duplicated at +32 for the same reason.
    int16_t m_upsampleBuffer[2][64]{};
    uint32_t m_resamplePos = 0; // 0..63, one full period == 2 44.1kHz ticks == 1 22.05kHz tick
};

} // namespace PsyX_SPUReverb

#endif // PSYX_SPUREVERB_H
