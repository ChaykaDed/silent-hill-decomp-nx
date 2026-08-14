// Standalone build:
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic
//     ../../pc_port/PsyCross/src/audio/PsyX_ReferenceXA.cpp
//     PsyX_ReferenceXA_test.cpp -o reference_xa_test

#include "../../src/audio/PsyX_ReferenceXA.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kOutputRate = 352800.0;
int gChecks = 0;
int gFailures = 0;

#define CHECK(condition) \
    do { \
        ++gChecks; \
        if (!(condition)) { \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", \
                         __FILE__, __LINE__, #condition); \
            ++gFailures; \
        } \
    } while (0)

uint64_t Expected(size_t frames, uint32_t rate)
{
    const uint64_t numerator = rate == 37800 ? 28u : 56u;
    return (static_cast<uint64_t>(frames) * numerator + 2u) / 3u;
}

std::vector<double> Convert(const std::vector<int16_t>& input,
                            uint32_t rate,
                            uint32_t channels,
                            const std::vector<size_t>& pushChunks,
                            const std::vector<size_t>& pullChunks)
{
    PsyX_ReferenceXA* src = PsyX_ReferenceXA_Create();
    CHECK(src != nullptr);
    const size_t frames = input.size() / channels;
    size_t pushed = 0;
    size_t pushIndex = 0;
    size_t pullIndex = 0;
    std::vector<double> output;

    while (pushed < frames)
    {
        const size_t amount = std::min(
            pushChunks[pushIndex++ % pushChunks.size()], frames - pushed);
        CHECK(PsyX_ReferenceXA_Push(
            src, input.data() + pushed * channels, amount, rate, channels) == 1);
        pushed += amount;

        const size_t request = pullChunks[pullIndex++ % pullChunks.size()];
        std::vector<double> block(request * 2);
        const size_t count =
            PsyX_ReferenceXA_PullStereo(src, block.data(), request);
        output.insert(output.end(), block.begin(), block.begin() + count * 2);
    }

    PsyX_ReferenceXA_Finish(src);
    while (!PsyX_ReferenceXA_IsDrained(src))
    {
        const size_t request = pullChunks[pullIndex++ % pullChunks.size()];
        std::vector<double> block(request * 2);
        const size_t count =
            PsyX_ReferenceXA_PullStereo(src, block.data(), request);
        CHECK(count != 0);
        output.insert(output.end(), block.begin(), block.begin() + count * 2);
    }
    CHECK(PsyX_ReferenceXA_OutputFrames(src) == Expected(frames, rate));
    CHECK(PsyX_ReferenceXA_InputFrames(src) == frames);
    PsyX_ReferenceXA_Destroy(src);
    return output;
}

std::vector<int16_t> MakeSignal(size_t frames, uint32_t channels)
{
    std::vector<int16_t> input(frames * channels);
    uint32_t state = 0x13579BDFu;
    for (int16_t& sample : input)
    {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<int16_t>(state >> 16);
    }
    return input;
}

double ToneMagnitude(const std::vector<double>& stereo,
                     size_t begin,
                     size_t end,
                     double frequency)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (size_t frame = begin; frame < end; ++frame)
    {
        const double angle =
            2.0 * kPi * frequency * static_cast<double>(frame) / kOutputRate;
        real += stereo[frame * 2] * std::cos(angle);
        imaginary -= stereo[frame * 2] * std::sin(angle);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) /
           static_cast<double>(end - begin);
}

void TestValidationAndRatios()
{
    PsyX_ReferenceXA* src = PsyX_ReferenceXA_Create();
    const int16_t sample[2] = { 1, 2 };
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 44100, 1) == 0);
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 37800, 3) == 0);
    CHECK(PsyX_ReferenceXA_InputFrames(src) == 0);
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 37800, 1) == 1);
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 18900, 1) == 0);
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 37800, 2) == 0);
    PsyX_ReferenceXA_Finish(src);
    CHECK(PsyX_ReferenceXA_Push(src, sample, 1, 37800, 1) == 0);
    CHECK(PsyX_ReferenceXA_ExpectedOutputFrames(src) == 10);
    PsyX_ReferenceXA_Destroy(src);

    for (uint32_t rate : { 37800u, 18900u })
    {
        for (size_t frames : { 1u, 2u, 3u, 17u, 300u, 1001u })
        {
            const auto output = Convert(MakeSignal(frames, 1), rate, 1,
                                        { 1, 7, 2, 31 }, { 1, 13, 5, 97 });
            CHECK(output.size() / 2 == Expected(frames, rate));
        }
    }
}

void TestChunkingDeterminismAndReset()
{
    for (uint32_t rate : { 37800u, 18900u })
    {
        const auto input = MakeSignal(731, 2);
        const auto one = Convert(input, rate, 2, { 731 }, { 100000 });
        const auto chunked = Convert(input, rate, 2, { 1, 19, 3, 64, 2 },
                                     { 7, 1, 113, 4 });
        CHECK(one.size() == chunked.size());
        CHECK(std::memcmp(one.data(), chunked.data(),
                          one.size() * sizeof(double)) == 0);

        PsyX_ReferenceXA* src = PsyX_ReferenceXA_Create();
        CHECK(PsyX_ReferenceXA_Push(src, input.data(), 731, rate, 2) == 1);
        PsyX_ReferenceXA_Finish(src);
        std::vector<double> first(one.size());
        CHECK(PsyX_ReferenceXA_PullStereo(src, first.data(), first.size() / 2) ==
              first.size() / 2);
        PsyX_ReferenceXA_Reset(src);
        CHECK(!PsyX_ReferenceXA_IsFinished(src));
        CHECK(PsyX_ReferenceXA_InputFrames(src) == 0);
        CHECK(PsyX_ReferenceXA_Push(src, input.data(), 731, rate, 2) == 1);
        PsyX_ReferenceXA_Finish(src);
        std::vector<double> second(one.size());
        CHECK(PsyX_ReferenceXA_PullStereo(
                  src, second.data(), second.size() / 2) == second.size() / 2);
        CHECK(std::memcmp(first.data(), second.data(),
                          first.size() * sizeof(double)) == 0);
        PsyX_ReferenceXA_Destroy(src);
    }
}

void TestDcAndChannels()
{
    for (uint32_t rate : { 37800u, 18900u })
    {
        std::vector<int16_t> mono(1200, 1234);
        const auto monoOut = Convert(mono, rate, 1, { 23, 1, 77 }, { 9, 101 });
        const size_t margin = 1400;
        for (size_t frame = margin; frame + margin < monoOut.size() / 2; ++frame)
        {
            CHECK(std::abs(monoOut[frame * 2] - 1234.0) < 1.0e-8);
            CHECK(monoOut[frame * 2] == monoOut[frame * 2 + 1]);
        }

        std::vector<int16_t> stereo(1200 * 2);
        for (size_t frame = 0; frame < 1200; ++frame)
        {
            stereo[frame * 2] = 2000;
            stereo[frame * 2 + 1] = -3000;
        }
        const auto stereoOut =
            Convert(stereo, rate, 2, { 11, 53 }, { 3, 211 });
        for (size_t frame = margin; frame + margin < stereoOut.size() / 2; ++frame)
        {
            CHECK(std::abs(stereoOut[frame * 2] - 2000.0) < 1.0e-8);
            CHECK(std::abs(stereoOut[frame * 2 + 1] + 3000.0) < 1.0e-8);
        }
    }
}

void TestImpulseAndTails()
{
    for (uint32_t rate : { 37800u, 18900u })
    {
        std::vector<int16_t> input(300, 0);
        input[150] = 32767;
        const auto output = Convert(input, rate, 1, { 300 }, { 17 });
        const size_t expectedCenter =
            static_cast<size_t>(150u * (rate == 37800 ? 28u : 56u) / 3u);
        size_t peak = 0;
        for (size_t frame = 1; frame < output.size() / 2; ++frame)
            if (std::abs(output[frame * 2]) > std::abs(output[peak * 2]))
                peak = frame;
        CHECK(peak == expectedCenter);
        CHECK(output[peak * 2] > 28000.0);
        CHECK(output[peak * 2] == output[peak * 2 + 1]);
        bool retainedFraction = false;
        for (size_t frame = 0; frame < output.size() / 2; ++frame)
            retainedFraction = retainedFraction ||
                std::floor(output[frame * 2]) != output[frame * 2];
        CHECK(retainedFraction);

        std::fill(input.begin(), input.end(), 0);
        input.back() = 32767;
        const auto tail = Convert(input, rate, 1, { 1, 299 }, { 2, 5 });
        CHECK(tail.size() / 2 == Expected(input.size(), rate));
        bool nonzeroTail = false;
        const size_t start = tail.size() / 2 > 20 ? tail.size() / 2 - 20 : 0;
        for (size_t frame = start; frame < tail.size() / 2; ++frame)
            nonzeroTail = nonzeroTail || tail[frame * 2] != 0.0;
        CHECK(nonzeroTail);
    }
}

void TestFrequencyAndStopband()
{
    for (uint32_t rate : { 37800u, 18900u })
    {
        const size_t frames = 4096;
        const double wantedFrequency = 1000.0;
        std::vector<int16_t> wanted(frames);
        std::vector<int16_t> stopped(frames);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            wanted[frame] = static_cast<int16_t>(12000.0 * std::sin(
                2.0 * kPi * wantedFrequency * static_cast<double>(frame) / rate));
            stopped[frame] = static_cast<int16_t>(12000.0 * std::sin(
                2.0 * kPi * 0.49 * static_cast<double>(frame)));
        }

        const auto wantedOut = Convert(wanted, rate, 1, { 4096 }, { 100000 });
        const auto stoppedOut = Convert(stopped, rate, 1, { 4096 }, { 100000 });
        const size_t margin = 1400;
        const size_t end = wantedOut.size() / 2 - margin;
        const double wantedMagnitude =
            ToneMagnitude(wantedOut, margin, end, wantedFrequency);
        CHECK(std::abs(wantedMagnitude - 12000.0) < 10.0);

        const double imageFrequency = static_cast<double>(rate) - wantedFrequency;
        const double imageMagnitude =
            ToneMagnitude(wantedOut, margin, end, imageFrequency);
        CHECK(imageMagnitude < wantedMagnitude * 1.0e-4);

        double stoppedEnergy = 0.0;
        double wantedEnergy = 0.0;
        for (size_t frame = margin; frame < end; ++frame)
        {
            stoppedEnergy += stoppedOut[frame * 2] * stoppedOut[frame * 2];
            wantedEnergy += wantedOut[frame * 2] * wantedOut[frame * 2];
        }
        CHECK(stoppedEnergy < wantedEnergy * 1.0e-4);

        std::vector<int16_t> edge(frames, -32768);
        std::fill(edge.begin() + frames / 2, edge.end(), 32767);
        const auto edgeOut = Convert(edge, rate, 1, { 37, 101 }, { 29, 3 });
        bool exceededS16 = false;
        for (double sample : edgeOut)
            exceededS16 = exceededS16 || sample > 32767.0 || sample < -32768.0;
        CHECK(exceededS16);
    }
}

void TestLazyGenerationAndQueueDepth()
{
    const auto input = MakeSignal(8064, 2);
    PsyX_ReferenceXA* src = PsyX_ReferenceXA_Create();
    CHECK(PsyX_ReferenceXA_Push(
        src, input.data(), input.size() / 2, 37800, 2) == 1);
    CHECK(PsyX_ReferenceXA_OutputFrames(src) == 0);

    const size_t queuedBefore = PsyX_ReferenceXA_QueuedFrames(src);
    CHECK(queuedBefore > 70000);
    std::vector<double> block(256 * 2);
    CHECK(PsyX_ReferenceXA_PullStereo(src, block.data(), 256) == 256);
    CHECK(PsyX_ReferenceXA_OutputFrames(src) == 256);
    CHECK(PsyX_ReferenceXA_QueuedFrames(src) == queuedBefore - 256);

    PsyX_ReferenceXA_Finish(src);
    CHECK(PsyX_ReferenceXA_OutputFrames(src) == 256);
    while (!PsyX_ReferenceXA_IsDrained(src))
        CHECK(PsyX_ReferenceXA_PullStereo(src, block.data(), 256) != 0);
    PsyX_ReferenceXA_Destroy(src);
}

} // namespace

int main()
{
    TestValidationAndRatios();
    TestChunkingDeterminismAndReset();
    TestDcAndChannels();
    TestImpulseAndTails();
    TestFrequencyAndStopband();
    TestLazyGenerationAndQueueDepth();
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
