#include "PsyX_ReferenceXA.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <vector>

namespace {

constexpr int kTaps = 128;
constexpr int kFirstTap = -63;
constexpr double kCutoff = 0.45;
constexpr double kKaiserBeta = 10.0;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct StereoFrame
{
    double left;
    double right;
};

double BesselI0(double x)
{
    const double y = x * x * 0.25;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= y / static_cast<double>(k * k);
        sum += term;
        if (term <= sum * 1.0e-17)
            break;
    }
    return sum;
}

double Sinc(double x)
{
    if (x == 0.0)
        return 1.0;
    const double angle = kPi * x;
    return std::sin(angle) / angle;
}

} // namespace

struct PsyX_ReferenceXA
{
    uint32_t sourceRateHz = 0;
    uint32_t channels = 0;
    uint32_t numerator = 0;
    uint32_t denominator = 3;
    uint64_t inputFrames = 0;
    uint64_t generatedFrames = 0;
    uint64_t pulledFrames = 0;
    uint64_t inputBase = 0;
    bool finished = false;
    std::deque<StereoFrame> input;
    std::deque<StereoFrame> output;
    std::vector<double> coefficients;
};

namespace {

bool ValidFormat(uint32_t sourceRateHz, uint32_t channels)
{
    return (sourceRateHz == 37800 || sourceRateHz == 18900) &&
           (channels == 1 || channels == 2);
}

uint64_t ExpectedFrames(const PsyX_ReferenceXA* src)
{
    if (!src || src->numerator == 0)
        return 0;
    const uint64_t quotient = src->inputFrames / src->denominator;
    const uint64_t remainder = src->inputFrames % src->denominator;
    return quotient * src->numerator +
           (remainder * src->numerator + src->denominator - 1) /
               src->denominator;
}

void BuildCoefficients(PsyX_ReferenceXA* src)
{
    src->coefficients.resize(static_cast<size_t>(src->numerator) * kTaps);
    const double windowDenominator = BesselI0(kKaiserBeta);

    for (uint32_t phase = 0; phase < src->numerator; ++phase)
    {
        const double fraction =
            static_cast<double>(phase) / static_cast<double>(src->numerator);
        double sum = 0.0;
        for (int tap = 0; tap < kTaps; ++tap)
        {
            const double offset = static_cast<double>(kFirstTap + tap);
            const double distance = fraction - offset;
            const double normalized = distance / 64.0;
            double coefficient = 0.0;
            if (std::abs(normalized) < 1.0)
            {
                const double window = BesselI0(
                    kKaiserBeta * std::sqrt(1.0 - normalized * normalized)) /
                    windowDenominator;
                coefficient = 2.0 * kCutoff *
                              Sinc(2.0 * kCutoff * distance) * window;
            }
            src->coefficients[static_cast<size_t>(phase) * kTaps + tap] =
                coefficient;
            sum += coefficient;
        }

        for (int tap = 0; tap < kTaps; ++tap)
            src->coefficients[static_cast<size_t>(phase) * kTaps + tap] /= sum;
    }
}

bool Configure(PsyX_ReferenceXA* src, uint32_t sourceRateHz, uint32_t channels)
{
    if (!ValidFormat(sourceRateHz, channels))
        return false;
    if (src->sourceRateHz != 0)
        return src->sourceRateHz == sourceRateHz && src->channels == channels;

    src->sourceRateHz = sourceRateHz;
    src->channels = channels;
    src->numerator = sourceRateHz == 37800 ? 28u : 56u;
    BuildCoefficients(src);
    return true;
}

StereoFrame InputAt(const PsyX_ReferenceXA* src, int64_t index)
{
    if (index < 0 || static_cast<uint64_t>(index) >= src->inputFrames)
        return { 0.0, 0.0 };
    const uint64_t relative = static_cast<uint64_t>(index) - src->inputBase;
    return src->input[static_cast<size_t>(relative)];
}

void DiscardOldInput(PsyX_ReferenceXA* src)
{
    const uint64_t positionNumerator =
        src->generatedFrames * src->denominator;
    const uint64_t center = positionNumerator / src->numerator;
    const int64_t firstNeeded =
        static_cast<int64_t>(center) + static_cast<int64_t>(kFirstTap);
    const uint64_t discardBefore =
        firstNeeded > 0 ? static_cast<uint64_t>(firstNeeded) : 0;
    while (src->inputBase < discardBefore && !src->input.empty())
    {
        src->input.pop_front();
        ++src->inputBase;
    }
}

uint64_t AvailableFrameLimit(const PsyX_ReferenceXA* src)
{
    if (src->numerator == 0)
        return 0;
    if (src->finished)
        return ExpectedFrames(src);
    if (src->inputFrames <= static_cast<uint64_t>(kFirstTap + kTaps - 1))
        return 0;

    const uint64_t usableCenters =
        src->inputFrames - static_cast<uint64_t>(kFirstTap + kTaps - 1);
    return (usableCenters * src->numerator + src->denominator - 1) /
           src->denominator;
}

void GenerateAvailable(PsyX_ReferenceXA* src, size_t targetQueuedFrames)
{
    if (src->numerator == 0)
        return;

    const uint64_t limit = AvailableFrameLimit(src);
    while (src->generatedFrames < limit &&
           src->output.size() < targetQueuedFrames)
    {
        const uint64_t positionNumerator =
            src->generatedFrames * src->denominator;
        const uint64_t center = positionNumerator / src->numerator;
        const uint32_t phase =
            static_cast<uint32_t>(positionNumerator % src->numerator);
        const int64_t lastNeeded =
            static_cast<int64_t>(center) + kFirstTap + kTaps - 1;
        if (!src->finished &&
            lastNeeded >= static_cast<int64_t>(src->inputFrames))
        {
            break;
        }

        StereoFrame result = { 0.0, 0.0 };
        const size_t coefficientBase = static_cast<size_t>(phase) * kTaps;
        for (int tap = 0; tap < kTaps; ++tap)
        {
            const int64_t inputIndex =
                static_cast<int64_t>(center) + kFirstTap + tap;
            const StereoFrame sample = InputAt(src, inputIndex);
            const double coefficient =
                src->coefficients[coefficientBase + static_cast<size_t>(tap)];
            result.left += sample.left * coefficient;
            result.right += sample.right * coefficient;
        }
        src->output.push_back(result);
        ++src->generatedFrames;
        DiscardOldInput(src);
    }
}

} // namespace

PsyX_ReferenceXA* PsyX_ReferenceXA_Create(void)
{
    return new PsyX_ReferenceXA();
}

void PsyX_ReferenceXA_Destroy(PsyX_ReferenceXA* src)
{
    delete src;
}

void PsyX_ReferenceXA_Reset(PsyX_ReferenceXA* src)
{
    if (!src)
        return;
    *src = PsyX_ReferenceXA();
}

int PsyX_ReferenceXA_Push(PsyX_ReferenceXA* src,
                          const int16_t* interleaved,
                          size_t frames,
                          uint32_t sourceRateHz,
                          uint32_t channels)
{
    if (!src || !interleaved || frames == 0 || src->finished ||
        !Configure(src, sourceRateHz, channels))
    {
        return 0;
    }
    if (frames > std::numeric_limits<uint64_t>::max() - src->inputFrames)
        return 0;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        const double left = static_cast<double>(interleaved[frame * channels]);
        const double right = channels == 2
            ? static_cast<double>(interleaved[frame * channels + 1])
            : left;
        src->input.push_back({ left, right });
    }
    src->inputFrames += static_cast<uint64_t>(frames);
    return 1;
}

void PsyX_ReferenceXA_Finish(PsyX_ReferenceXA* src)
{
    if (!src || src->finished)
        return;
    src->finished = true;
}

size_t PsyX_ReferenceXA_PullStereo(PsyX_ReferenceXA* src,
                                   double* interleavedStereo,
                                   size_t maxFrames)
{
    if (!src || !interleavedStereo || maxFrames == 0)
        return 0;
    GenerateAvailable(src, maxFrames);
    const size_t count = std::min(maxFrames, src->output.size());
    for (size_t frame = 0; frame < count; ++frame)
    {
        interleavedStereo[frame * 2] = src->output.front().left;
        interleavedStereo[frame * 2 + 1] = src->output.front().right;
        src->output.pop_front();
    }
    src->pulledFrames += static_cast<uint64_t>(count);
    return count;
}

size_t PsyX_ReferenceXA_QueuedFrames(const PsyX_ReferenceXA* src)
{
    if (!src)
        return 0;
    const uint64_t limit = AvailableFrameLimit(src);
    const uint64_t queued =
        limit > src->pulledFrames ? limit - src->pulledFrames : 0;
    return static_cast<size_t>(std::min<uint64_t>(
        queued, std::numeric_limits<size_t>::max()));
}

uint64_t PsyX_ReferenceXA_InputFrames(const PsyX_ReferenceXA* src)
{
    return src ? src->inputFrames : 0;
}

uint64_t PsyX_ReferenceXA_OutputFrames(const PsyX_ReferenceXA* src)
{
    return src ? src->generatedFrames : 0;
}

uint64_t PsyX_ReferenceXA_ExpectedOutputFrames(const PsyX_ReferenceXA* src)
{
    return ExpectedFrames(src);
}

int PsyX_ReferenceXA_IsFinished(const PsyX_ReferenceXA* src)
{
    return src && src->finished ? 1 : 0;
}

int PsyX_ReferenceXA_IsDrained(const PsyX_ReferenceXA* src)
{
    return src && src->finished && src->output.empty() &&
           src->pulledFrames == ExpectedFrames(src) ? 1 : 0;
}
