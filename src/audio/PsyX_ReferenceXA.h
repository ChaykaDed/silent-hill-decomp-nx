#ifndef PSYX_REFERENCEXA_H
#define PSYX_REFERENCEXA_H

#include <stddef.h>
#include <stdint.h>

#define PSYX_REFERENCEXA_OUTPUT_RATE_HZ 352800u

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef struct PsyX_ReferenceXA PsyX_ReferenceXA;

PsyX_ReferenceXA* PsyX_ReferenceXA_Create(void);
void PsyX_ReferenceXA_Destroy(PsyX_ReferenceXA* src);
void PsyX_ReferenceXA_Reset(PsyX_ReferenceXA* src);

int PsyX_ReferenceXA_Push(PsyX_ReferenceXA* src,
                          const int16_t* interleaved,
                          size_t frames,
                          uint32_t sourceRateHz,
                          uint32_t channels);
void PsyX_ReferenceXA_Finish(PsyX_ReferenceXA* src);

size_t PsyX_ReferenceXA_PullStereo(PsyX_ReferenceXA* src,
                                   double* interleavedStereo,
                                   size_t maxFrames);
size_t PsyX_ReferenceXA_QueuedFrames(const PsyX_ReferenceXA* src);
uint64_t PsyX_ReferenceXA_InputFrames(const PsyX_ReferenceXA* src);
uint64_t PsyX_ReferenceXA_OutputFrames(const PsyX_ReferenceXA* src);
uint64_t PsyX_ReferenceXA_ExpectedOutputFrames(const PsyX_ReferenceXA* src);
int PsyX_ReferenceXA_IsFinished(const PsyX_ReferenceXA* src);
int PsyX_ReferenceXA_IsDrained(const PsyX_ReferenceXA* src);

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
