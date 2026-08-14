#ifndef PSYX_XASTREAM_H
#define PSYX_XASTREAM_H

#include <stdint.h>
#include <stddef.h>

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

typedef struct PsyX_XAStream PsyX_XAStream;

typedef struct PsyX_XAStreamStats {
    uint32_t queuedBlocks;          /* queued FIFO blocks */
    uint32_t queuedInputFrames;     /* unread input frames */
    uint32_t queuedOutputFrames;    /* output frames still available */
    uint32_t consumedInputFrames;   /* total input frames consumed */
    uint32_t producedOutputFrames;   /* total output frames produced */
    uint32_t droppedBlocks;         /* rejected pushes */
    uint32_t resetCount;
    uint32_t drainCount;
    uint32_t sourceRateHz;          /* 0 when idle */
    uint32_t channels;              /* 0 when idle */
    uint32_t fifoCapacityBlocks;
} PsyX_XAStreamStats;

/* Caller-owned locking: the adapter must serialize all calls with one SDL mutex.
 * No internal platform mutexes are used here. */
PsyX_XAStream* PsyX_XAStream_Create(void);
void PsyX_XAStream_Destroy(PsyX_XAStream* s);
void PsyX_XAStream_Reset(PsyX_XAStream* s);
void PsyX_XAStream_Drain(PsyX_XAStream* s);

int PsyX_XAStream_Push(PsyX_XAStream* s, const int16_t* interleaved, uint32_t frames, uint32_t sourceHz, uint32_t channels);
uint32_t PsyX_XAStream_Pop44100Stereo(PsyX_XAStream* s, int16_t* outInterleavedStereo, uint32_t maxFrames);
uint32_t PsyX_XAStream_PeekQueuedOutputFrames(const PsyX_XAStream* s);
void PsyX_XAStream_GetStats(const PsyX_XAStream* s, PsyX_XAStreamStats* out);

#ifdef PSYX_XASTREAM_SELFTEST
int PsyX_XAStream_SelfTest(void);
#endif

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif
