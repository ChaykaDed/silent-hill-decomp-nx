// PsyX_ReferenceResampler_test.cpp
//
// Standalone, dependency-free test/verification harness for
// PsyX_ReferenceResampler (see pc_port/PsyCross/src/audio/PsyX_ReferenceResampler.h).
//
// No external test framework is used (matches the module's own "no
// external dependencies" requirement): this file is a plain main() with a
// handful of small check helpers, buildable/runnable completely standalone
// (see the "Build standalone" instructions below). It exits with status 0
// if every check passes, and a nonzero status (with a printed summary of
// which check(s) failed) otherwise.
//
// Build standalone (no CMake/build-system integration required):
//   g++ -std=c++17 -O2 -Wall -Wextra -I ../../pc_port/PsyCross/src/audio
//       PsyX_ReferenceResampler_test.cpp
//       ../../pc_port/PsyCross/src/audio/PsyX_ReferenceResampler.cpp
//       -o PsyX_ReferenceResampler_test
//   ./PsyX_ReferenceResampler_test
//
// (or the equivalent cl.exe / clang++ invocation - there is nothing here
// that depends on a particular compiler or build system).

#include "../../src/audio/PsyX_ReferenceResampler.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdint>

using PsyX::ReferenceResampler;

namespace
{

int  g_checksRun    = 0;
int  g_checksFailed = 0;

void CheckImpl(bool condition, const char* expr, const char* file, int line)
{
    ++g_checksRun;
    if (!condition)
    {
        ++g_checksFailed;
        std::printf("  [FAIL] %s (%s:%d)\n", expr, file, line);
    }
}

#define CHECK(cond) CheckImpl((cond), #cond, __FILE__, __LINE__)

bool NearlyEqual(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

// Deterministic pseudo-noise-ish test signal - a fixed sum of a few
// incommensurate sinusoids. No rand()/time-seeding involved, so it is
// bit-for-bit identical on every run/process.
std::vector<int16_t> MakeTestSignal(int n)
{
    std::vector<int16_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const double v = 9000.0 * std::sin(i * 0.1372) + 4000.0 * std::sin(i * 0.0451 + 0.7) + 1500.0 * std::sin(i * 0.911);
        out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(v));
    }
    return out;
}

std::vector<int16_t> MakeSine(int n, double freqHz, double sampleRate, double amplitude, double phase = 0.0)
{
    std::vector<int16_t> out(static_cast<size_t>(n));
    const double         w = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
    for (int i = 0; i < n; ++i)
    {
        const double v = amplitude * std::sin(w * i + phase);
        out[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(v));
    }
    return out;
}

double RMS(const std::vector<double>& v, size_t start, size_t count)
{
    double sum = 0.0;
    for (size_t i = start; i < start + count && i < v.size(); ++i)
        sum += v[i] * v[i];
    return std::sqrt(sum / static_cast<double>(count));
}

// Counts positive-going zero crossings in v[start, start+count) and uses
// them to estimate the dominant frequency, given the sample rate the
// buffer was produced at. Adequate for a single, dominant sinusoid (used
// only for the frequency-accuracy checks below).
double EstimateFrequencyHz(const std::vector<double>& v, size_t start, size_t count, double sampleRate)
{
    int crossings = 0;
    for (size_t i = start + 1; i < start + count && i < v.size(); ++i)
    {
        if (v[i - 1] < 0.0 && v[i] >= 0.0)
            ++crossings;
    }
    const double durationSec = static_cast<double>(count) / sampleRate;
    return static_cast<double>(crossings) / durationSec;
}

// ---------------------------------------------------------------------
// 1. Impulse response
// ---------------------------------------------------------------------
void Test_Impulse()
{
    std::printf("Test_Impulse\n");

    std::vector<int16_t> signal(512, 0);
    signal[200] = 32767;

    ReferenceResampler r;
    // Pre-roll some silence so the impulse is not at the very start of
    // the history buffer (avoids the "before start of stream" zero-pad
    // edge from masking a real filter tap).
    r.PushSamples(signal.data(), signal.size());

    const uint64_t step = ReferenceResampler::PitchToStep(ReferenceResampler::kSpuPitchUnity);

    std::vector<double> out(512 * ReferenceResampler::kOutputOversample);
    for (size_t j = 0; j < out.size(); ++j)
        out[j] = r.ProcessDouble(step);

    // At unity pitch (R==1, full-bandwidth bucket), every 8th output
    // sample lands exactly on an integer source position (phase==0),
    // where the polyphase filter is an EXACT passthrough (see header
    // notes: sinc(nonzero integer)==0, all non-center taps vanish, and
    // the per-phase DC-normalization forces the single nonzero tap to
    // exactly 1.0). The impulse is at source index 200, so output index
    // 200*8 == 1600 must reproduce it exactly, and every other phase-0 tap
    // must be exactly zero.
    const size_t impulseOutIndex = 200 * ReferenceResampler::kOutputOversample;
    CHECK(NearlyEqual(out[impulseOutIndex], 32767.0, 1e-6));

    for (size_t j = 0; j < out.size(); j += ReferenceResampler::kOutputOversample)
    {
        if (j == impulseOutIndex)
            continue;
        CHECK(NearlyEqual(out[j], 0.0, 1e-6));
    }

    // The interpolated (non-phase-0) points around the impulse should be
    // bounded (no blow-up / instability) - a windowed-sinc impulse
    // response has bounded Gibbs overshoot, comfortably under 2x here.
    double peak = 0.0;
    for (double v : out)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak < 32767.0 * 2.0);

    // And it must be echo-free (finite support): far enough away from
    // the impulse (beyond kHalfTaps+1 source samples), the response must
    // be exactly zero.
    const size_t farOutIndex = (200 + ReferenceResampler::kHalfTaps + 5) * ReferenceResampler::kOutputOversample;
    if (farOutIndex < out.size())
        CHECK(NearlyEqual(out[farOutIndex], 0.0, 1e-6));
}

// ---------------------------------------------------------------------
// 2. DC passthrough / unity DC normalization
// ---------------------------------------------------------------------
void Test_DC()
{
    std::printf("Test_DC\n");

    const int16_t         dcValue = 12345;
    std::vector<int16_t>  signal(2000, dcValue);

    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());

    // Exercise several pitches (including modulation-like changes) to
    // confirm DC survives regardless of bandlimit bucket. The slowest
    // pitch here (0x0800, R==0.5) advances the source position only
    // 0.0625 samples per output tick on the 8x clock, so it needs a much
    // longer warm-up (in output ticks) than the faster pitches to get the
    // filter window fully clear of the start-of-stream zero-padding.
    const uint32_t pitches[] = { 0x0800u, 0x1000u, 0x1800u, 0x2000u, 0x4000u, 0x8000u };
    for (uint32_t pitch : pitches)
    {
        ReferenceResampler rr;
        rr.PushSamples(signal.data(), signal.size());
        const uint64_t step = ReferenceResampler::PitchToStep(pitch);

        // Prime by source position rather than a fixed output-tick count,
        // so this remains correct for every pitch and oversample factor.
        while ((rr.GetPosition() >> ReferenceResampler::kPosFracBits) <
               static_cast<uint64_t>(ReferenceResampler::kHalfTaps))
            rr.ProcessDouble(step);

        for (int i = 0; i < 30; ++i)
        {
            const double v = rr.ProcessDouble(step);
            CHECK(NearlyEqual(v, static_cast<double>(dcValue), 1.0));
        }
    }
}

// ---------------------------------------------------------------------
// 3. Unity pitch exact passthrough at aligned phase
// ---------------------------------------------------------------------
void Test_UnityPitch()
{
    std::printf("Test_UnityPitch\n");

    const int                   n      = 300;
    const std::vector<int16_t> signal = MakeTestSignal(n);

    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());

    const uint64_t step = ReferenceResampler::PitchToStep(ReferenceResampler::kSpuPitchUnity);

    for (int i = 0; i < n; ++i)
    {
        const double v = r.ProcessDouble(step);
        if (i % ReferenceResampler::kOutputOversample == 0)
        {
            const int srcIndex = i / ReferenceResampler::kOutputOversample;
            CHECK(NearlyEqual(v, static_cast<double>(signal[static_cast<size_t>(srcIndex)]), 1e-6));
        }
    }
}

// ---------------------------------------------------------------------
// 4. Fractional (non power-of-two) pitch stability + rough frequency track
// ---------------------------------------------------------------------
void Test_FractionalPitch()
{
    std::printf("Test_FractionalPitch\n");

    const double freqHz = 500.0;
    const int    n       = 4000;
    const auto   signal  = MakeSine(n, freqHz, ReferenceResampler::kSourceSampleRate, 8000.0);

    // 0x1234 / 0x1000 == 1.1416015625x - an arbitrary fractional pitch.
    const uint32_t pitch = 0x1234u;
    const uint64_t step  = ReferenceResampler::PitchToStep(pitch);
    const double   ratio = static_cast<double>(pitch) / static_cast<double>(ReferenceResampler::kSpuPitchUnity);

    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());

    std::vector<double> out;
    out.reserve(static_cast<size_t>(n) * ReferenceResampler::kOutputOversample);
    const int totalOut = static_cast<int>((n - 200) * ReferenceResampler::kOutputOversample / ratio);
    for (int i = 0; i < totalOut; ++i)
        out.push_back(r.ProcessDouble(step));

    // Bounded, no blow-up.
    double peak = 0.0;
    for (double v : out)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak < 8000.0 * 1.5);

    // Frequency should have shifted up by `ratio` (pitch-shifting is
    // exactly what a variable-rate reader does), measured away from the
    // start/end warm-up regions.
    const size_t warm         = out.size() / 8;
    const double measuredFreq = EstimateFrequencyHz(out, warm, out.size() - 2 * warm, ReferenceResampler::kOutputSampleRate);
    const double expectedFreq = freqHz * ratio;
    CHECK(NearlyEqual(measuredFreq, expectedFreq, expectedFreq * 0.05));
}

// ---------------------------------------------------------------------
// 5. Frequency accuracy at unity pitch
// ---------------------------------------------------------------------
void Test_FrequencyAccuracy()
{
    std::printf("Test_FrequencyAccuracy\n");

    const double freqHz = 1000.0;
    const int    n       = 4000;
    const auto   signal  = MakeSine(n, freqHz, ReferenceResampler::kSourceSampleRate, 10000.0);

    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());

    const uint64_t step = ReferenceResampler::PitchToStep(ReferenceResampler::kSpuPitchUnity);

    std::vector<double> out(static_cast<size_t>(n) * ReferenceResampler::kOutputOversample);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = r.ProcessDouble(step);

    const size_t warm         = out.size() / 8;
    const double measuredFreq = EstimateFrequencyHz(out, warm, out.size() - 2 * warm, ReferenceResampler::kOutputSampleRate);
    CHECK(NearlyEqual(measuredFreq, freqHz, freqHz * 0.02));

    // Amplitude should be preserved (unity-gain reconstruction): compare
    // RMS of a steady-state window to the theoretical sine RMS.
    const double measuredRms  = RMS(out, warm, out.size() - 2 * warm);
    const double expectedRms  = 10000.0 / std::sqrt(2.0);
    CHECK(NearlyEqual(measuredRms, expectedRms, expectedRms * 0.05));
}

// ---------------------------------------------------------------------
// 6. High-pitch anti-aliasing
// ---------------------------------------------------------------------
void Test_HighPitchAntiAliasing()
{
    std::printf("Test_HighPitchAntiAliasing\n");

    // A tone close to the source Nyquist (22050Hz @ 44100Hz source rate).
    const double freqHz = 18000.0;
    const int    n       = 4000;
    const auto   signal  = MakeSine(n, freqHz, ReferenceResampler::kSourceSampleRate, 10000.0);

    // R == 8 (96 1/32-octave bins): cutoff == 0.8/8 == 0.1 of source Nyquist
    // == 2205Hz. 18000Hz is far above that, so it must be heavily
    // attenuated by the bandlimit bucket before being pitch-shifted up
    // (where it would otherwise alias down into an audible garbage tone).
    const uint32_t pitch = ReferenceResampler::kSpuPitchUnity * 8u;
    const uint64_t step  = ReferenceResampler::PitchToStep(pitch);

    const int bin = 3 * ReferenceResampler::kBandlimitBinsPerOctave;
    CHECK(ReferenceResampler::BucketForStep(step) == bin);
    CHECK(NearlyEqual(ReferenceResampler::DesignCutoffForBucket(bin), 0.1, 1e-9));

    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());

    std::vector<double> out;
    const int totalOut = static_cast<int>((n - 200) * ReferenceResampler::kOutputOversample / 8.0);
    out.reserve(static_cast<size_t>(totalOut));
    for (int i = 0; i < totalOut; ++i)
        out.push_back(r.ProcessDouble(step));

    const size_t warm        = out.size() / 8;
    const double outRms      = RMS(out, warm, out.size() - 2 * warm);
    const double inputRms    = 10000.0 / std::sqrt(2.0);

    // Without bandlimiting the output RMS would track the input RMS
    // (~7071). With correct anti-aliasing, the 18kHz tone is well outside
    // the ~2205Hz design passband and must be suppressed to a small
    // fraction of the input amplitude.
    CHECK(outRms < inputRms * 0.15);
}

void Test_NearUnityBandwidth()
{
    std::printf("Test_NearUnityBandwidth\n");

    const uint32_t pitch = ReferenceResampler::kSpuPitchUnity + 1u;
    const uint64_t step = ReferenceResampler::PitchToStep(pitch);
    const int bin = ReferenceResampler::BucketForStep(step);
    const double ratio = static_cast<double>(pitch) /
                         static_cast<double>(ReferenceResampler::kSpuPitchUnity);
    const double cutoff = ReferenceResampler::DesignCutoffForBucket(bin);
    CHECK(bin == 1);
    CHECK(cutoff > 0.77);
    CHECK(cutoff <= 0.80 / ratio);

    const int n = 6000;
    const auto signal = MakeSine(
        n, 14000.0, ReferenceResampler::kSourceSampleRate, 10000.0);
    ReferenceResampler r;
    r.PushSamples(signal.data(), signal.size());
    std::vector<double> out(static_cast<size_t>(n - 200) *
                            ReferenceResampler::kOutputOversample);
    for (double& sample : out)
        sample = r.ProcessDouble(step);
    const size_t warm = out.size() / 8;
    const double outRms = RMS(out, warm, out.size() - 2 * warm);
    CHECK(outRms > (10000.0 / std::sqrt(2.0)) * 0.80);
}

// ---------------------------------------------------------------------
// 7. Determinism
// ---------------------------------------------------------------------
void Test_Determinism()
{
    std::printf("Test_Determinism\n");

    const int  n      = 1000;
    const auto signal = MakeTestSignal(n);

    // Simulated pitch modulation: a slowly varying step sequence.
    std::vector<uint64_t> steps(2000);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const uint32_t pitch = ReferenceResampler::kSpuPitchUnity + static_cast<uint32_t>(200.0 * std::sin(i * 0.01));
        steps[i]              = ReferenceResampler::PitchToStep(pitch);
    }

    auto run = [&]() {
        ReferenceResampler r;
        r.PushSamples(signal.data(), signal.size());
        std::vector<double> out(steps.size());
        for (size_t i = 0; i < steps.size(); ++i)
            out[i] = r.ProcessDouble(steps[i]);
        return out;
    };

    const std::vector<double> a = run();
    const std::vector<double> b = run();

    CHECK(a.size() == b.size());
    bool identical = (a.size() == b.size());
    for (size_t i = 0; identical && i < a.size(); ++i)
    {
        if (std::memcmp(&a[i], &b[i], sizeof(double)) != 0)
        {
            identical = false;
            std::printf("  mismatch at %zu: %.17g vs %.17g\n", i, a[i], b[i]);
        }
    }
    CHECK(identical);

    // A third, freshly-run instance created AFTER the coefficient banks
    // used above are already cached (process-wide) must still produce
    // the same result - the cache must not introduce any state leakage.
    const std::vector<double> c = run();
    CHECK(c.size() == a.size());
    bool identicalC = (c.size() == a.size());
    for (size_t i = 0; identicalC && i < a.size(); ++i)
        identicalC = identicalC && (std::memcmp(&a[i], &c[i], sizeof(double)) == 0);
    CHECK(identicalC);
}

// ---------------------------------------------------------------------
// 8. Continuous history across (ADPCM-block-sized) chunks
// ---------------------------------------------------------------------
void Test_ContinuousHistoryAcrossBlocks()
{
    std::printf("Test_ContinuousHistoryAcrossBlocks\n");

    const int  n      = 20 * ReferenceResampler::kAdpcmBlockSamples; // 560 samples, 20 "ADPCM blocks"
    const auto signal = MakeTestSignal(n);

    const uint32_t pitch = 0x1300u; // arbitrary non-trivial fractional pitch
    const uint64_t step  = ReferenceResampler::PitchToStep(pitch);
    const double   ratio = static_cast<double>(pitch) / static_cast<double>(ReferenceResampler::kSpuPitchUnity);
    const int      maxOut = static_cast<int>((n - 2 * ReferenceResampler::kHalfTaps) * ReferenceResampler::kOutputOversample / ratio);

    // Baseline: entire signal pushed in one shot.
    ReferenceResampler baseline;
    baseline.PushSamples(signal.data(), signal.size());
    std::vector<double> baselineOut(static_cast<size_t>(maxOut));
    for (int i = 0; i < maxOut; ++i)
        baselineOut[static_cast<size_t>(i)] = baseline.ProcessDouble(step);

    // Streaming: pushed one SPU-ADPCM block (28 samples) at a time,
    // draining as many output samples as are safely available after each
    // push (i.e. simulating real block-on-demand ADPCM decode feeding a
    // live resampler), rather than pre-loading everything up front.
    ReferenceResampler streaming;
    std::vector<double> streamingOut;
    streamingOut.reserve(static_cast<size_t>(maxOut));

    const int blockSize = ReferenceResampler::kAdpcmBlockSamples;
    for (int pushed = 0; pushed < n; pushed += blockSize)
    {
        const int count = std::min(blockSize, n - pushed);
        streaming.PushSamples(signal.data() + pushed, static_cast<size_t>(count));

        while (static_cast<int>(streamingOut.size()) < maxOut)
        {
            const int64_t nPos = static_cast<int64_t>(streaming.GetPosition() >> ReferenceResampler::kPosFracBits);
            if (nPos + ReferenceResampler::kHalfTaps + 1 >= streaming.GetHistoryEndIndex())
                break; // not enough look-ahead pushed yet - wait for the next block
            streamingOut.push_back(streaming.ProcessDouble(step));
        }
    }
    // Drain whatever remains now that all blocks have been pushed.
    while (static_cast<int>(streamingOut.size()) < maxOut)
        streamingOut.push_back(streaming.ProcessDouble(step));

    CHECK(streamingOut.size() == baselineOut.size());
    bool identical = (streamingOut.size() == baselineOut.size());
    for (size_t i = 0; identical && i < baselineOut.size(); ++i)
    {
        if (std::memcmp(&streamingOut[i], &baselineOut[i], sizeof(double)) != 0)
        {
            identical = false;
            std::printf("  mismatch at %zu: streaming=%.17g baseline=%.17g\n", i, streamingOut[i], baselineOut[i]);
        }
    }
    CHECK(identical);
}

// ---------------------------------------------------------------------
// 9. HalfBandDecimator: DC gain (unity) and frequency-selective response
// ---------------------------------------------------------------------
void Test_HalfBandDecimator_DC()
{
    std::printf("Test_HalfBandDecimator_DC\n");

    PsyX::HalfBandDecimator dec;
    const double dcValue = 12345.0;

    // Warm up past the fixed kMaxOffset-input-sample group delay (see
    // class doc comment) before checking the steady-state output.
    double last = 0.0;
    for (int i = 0; i < 100; ++i)
        last = dec.Process(dcValue, dcValue);

    for (int i = 0; i < 20; ++i)
    {
        last = dec.Process(dcValue, dcValue);
        CHECK(NearlyEqual(last, dcValue, 1e-6));
    }
}

void Test_HalfBandDecimator_FrequencyResponse()
{
    std::printf("Test_HalfBandDecimator_FrequencyResponse\n");

    const double inputRate = 352800.0;
    const int    n         = 4000;

    // Low tone: well inside the nc==0.5 half-band passband (well below the
    // new, halved Nyquist) - must survive decimation at close to unity gain.
    const double         lowFreq    = 0.1 * (inputRate / 2.0);
    const std::vector<int16_t> lowSignal  = MakeSine(n, lowFreq, inputRate, 10000.0);

    // High tone: above the NEW (post-decimation) Nyquist but still below
    // the OLD one, i.e. exactly the content a half-band decimator exists
    // to remove before subsampling - must be heavily attenuated so it can
    // never alias into the decimated output.
    const double         highFreq   = 0.45 * inputRate;
    const std::vector<int16_t> highSignal = MakeSine(n, highFreq, inputRate, 10000.0);

    PsyX::HalfBandDecimator decLow, decHigh;
    std::vector<double> outLow, outHigh;
    for (int i = 0; i + 1 < n; i += 2)
    {
        outLow.push_back(decLow.Process(static_cast<double>(lowSignal[static_cast<size_t>(i)]),
                                         static_cast<double>(lowSignal[static_cast<size_t>(i + 1)])));
        outHigh.push_back(decHigh.Process(static_cast<double>(highSignal[static_cast<size_t>(i)]),
                                           static_cast<double>(highSignal[static_cast<size_t>(i + 1)])));
    }

    const size_t warm      = outLow.size() / 8;
    const double lowRms    = RMS(outLow, warm, outLow.size() - 2 * warm);
    const double highRms   = RMS(outHigh, warm, outHigh.size() - 2 * warm);
    const double inputRms  = 10000.0 / std::sqrt(2.0);

    CHECK(NearlyEqual(lowRms, inputRms, inputRms * 0.05));
    CHECK(highRms < inputRms * 0.15);
}

// ---------------------------------------------------------------------
// 10. ReferenceOutputDecimator: stage mapping, cadence, DC, determinism
// ---------------------------------------------------------------------
void Test_ReferenceOutputDecimator_StageMapping()
{
    std::printf("Test_ReferenceOutputDecimator_StageMapping\n");

    using PsyX::ReferenceOutputDecimator;
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(352800) == 0);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(176400) == 1);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(88200) == 2);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(44100) == 3);

    // Never an arbitrary/interpolated rate - anything off the direct
    // 352800/2^n family must be clearly rejected (-1), not silently
    // mapped to some nearby stage count.
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(48000) == -1);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(22050) == -1);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(96000) == -1);
    CHECK(ReferenceOutputDecimator::StagesForOutputRate(0) == -1);
}

void Test_ReferenceOutputDecimator_PassthroughStage0()
{
    std::printf("Test_ReferenceOutputDecimator_PassthroughStage0\n");

    PsyX::ReferenceOutputDecimator dec;
    dec.Reset(0);
    for (int i = 0; i < 50; ++i)
    {
        const double inL = static_cast<double>(i) * 1.5;
        const double inR = static_cast<double>(i) * -2.5;
        double       outL = 0.0, outR = 0.0;
        // Stage count 0 (352800Hz direct device rate) is pure passthrough:
        // every call must be immediately ready, with no filtering at all.
        CHECK(dec.Process(inL, inR, &outL, &outR));
        CHECK(outL == inL);
        CHECK(outR == inR);
    }
}

void Test_ReferenceOutputDecimator_Cadence()
{
    std::printf("Test_ReferenceOutputDecimator_Cadence\n");

    // A stageCount-stage cascade must produce exactly one ready output per
    // 2^stageCount input calls (176400/88200/44100 == 1/2/3 stages - see
    // file header), independent of the internal per-stage pending/history
    // bookkeeping.
    for (int stageCount = 1; stageCount <= 3; ++stageCount)
    {
        PsyX::ReferenceOutputDecimator dec;
        dec.Reset(stageCount);

        const int callCount = 800;
        int       readyCount = 0;
        for (int i = 0; i < callCount; ++i)
        {
            double outL = 0.0, outR = 0.0;
            const double v = static_cast<double>(i);
            if (dec.Process(v, -v, &outL, &outR))
                ++readyCount;
        }

        const int expected = callCount / (1 << stageCount);
        CHECK(readyCount == expected);
    }
}

void Test_ReferenceOutputDecimator_DC()
{
    std::printf("Test_ReferenceOutputDecimator_DC\n");

    for (int stageCount = 0; stageCount <= 3; ++stageCount)
    {
        PsyX::ReferenceOutputDecimator dec;
        dec.Reset(stageCount);

        const double dcL = 8000.0;
        const double dcR = -6000.0;
        double       lastL = 0.0, lastR = 0.0;
        // Generously overestimated warm-up: enough calls to clear every
        // stage's group delay several times over before checking steady
        // state, regardless of stageCount.
        for (int i = 0; i < 4000; ++i)
        {
            double outL = 0.0, outR = 0.0;
            if (dec.Process(dcL, dcR, &outL, &outR))
            {
                lastL = outL;
                lastR = outR;
            }
        }
        CHECK(NearlyEqual(lastL, dcL, 1e-6));
        CHECK(NearlyEqual(lastR, dcR, 1e-6));
    }
}

void Test_ReferenceOutputDecimator_Determinism()
{
    std::printf("Test_ReferenceOutputDecimator_Determinism\n");

    auto run = [](int stageCount) {
        PsyX::ReferenceOutputDecimator dec;
        dec.Reset(stageCount);
        std::vector<double> out;
        for (int i = 0; i < 2000; ++i)
        {
            const double v = 4000.0 * std::sin(i * 0.0731) + 1000.0 * std::sin(i * 0.233 + 0.4);
            double       outL = 0.0, outR = 0.0;
            if (dec.Process(v, -v, &outL, &outR))
            {
                out.push_back(outL);
                out.push_back(outR);
            }
        }
        return out;
    };

    for (int stageCount = 0; stageCount <= 3; ++stageCount)
    {
        const std::vector<double> a = run(stageCount);
        const std::vector<double> b = run(stageCount);
        CHECK(a.size() == b.size());
        bool identical = (a.size() == b.size());
        for (size_t i = 0; identical && i < a.size(); ++i)
            identical = identical && (std::memcmp(&a[i], &b[i], sizeof(double)) == 0);
        CHECK(identical);
    }
}

} // namespace

int main()
{
    ReferenceResampler::WarmUpAllBanks();

    Test_Impulse();
    Test_DC();
    Test_UnityPitch();
    Test_FractionalPitch();
    Test_FrequencyAccuracy();
    Test_HighPitchAntiAliasing();
    Test_NearUnityBandwidth();
    Test_Determinism();
    Test_ContinuousHistoryAcrossBlocks();
    Test_HalfBandDecimator_DC();
    Test_HalfBandDecimator_FrequencyResponse();
    Test_ReferenceOutputDecimator_StageMapping();
    Test_ReferenceOutputDecimator_PassthroughStage0();
    Test_ReferenceOutputDecimator_Cadence();
    Test_ReferenceOutputDecimator_DC();
    Test_ReferenceOutputDecimator_Determinism();

    std::printf("\n%d checks run, %d failed\n", g_checksRun, g_checksFailed);
    return g_checksFailed == 0 ? 0 : 1;
}
