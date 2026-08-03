#ifndef PAREAD_H
#define PAREAD_H

#include <stdio.h>
#include <stdlib.h>
#include <portaudio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <math.h>
#include <float.h>
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"
#include "helper.h"


#define CHANNELS 2
#define MAX_TIMING_EVENTS 65536

/*
 * Must be a power of two.
 *
 * 65,536 samples provides about 8.2 seconds of buffering
 * at 8 kHz.
 */
#define SAMPLE_QUEUE_CAPACITY (1u << 16)
#define SAMPLE_QUEUE_MASK     (SAMPLE_QUEUE_CAPACITY - 1u)

_Static_assert(
    (SAMPLE_QUEUE_CAPACITY & (SAMPLE_QUEUE_CAPACITY - 1u)) == 0,
    "SAMPLE_QUEUE_CAPACITY must be a power of two"
);

/*
 * One sample from one signal conditioner.
 *
 * Keep the values as raw int16 samples in the callback.
 * Conversion to acceleration happens in the publisher thread.
 */
typedef struct {
    double adcTime;
    uint64_t sequence;
    int16_t channel[CHANNELS];
} QueuedSample;

/*
 * Single-producer/single-consumer queue.
 *
 * Producer:
 *     one PortAudio callback
 *
 * Consumer:
 *     publisher thread
 */
typedef struct {
    QueuedSample samples[SAMPLE_QUEUE_CAPACITY];

    _Atomic uint64_t writeIndex;
    _Atomic uint64_t readIndex;

    _Atomic uint64_t fullDropCount;
} SampleQueue;

typedef struct {
    IMAGE *img;

    SampleQueue *sc0Queue;
    SampleQueue *sc1Queue;

    float sc0Scale[CHANNELS];
    float sc1Scale[CHANNELS];

    /*
     * Maximum timestamp difference allowed when pairing samples.
     */
    double matchToleranceSeconds;

    _Atomic int stopRequested;

    uint64_t publishedFrames;
    uint64_t unmatchedSc0;
    uint64_t unmatchedSc1;

    double maximumAbsoluteSkewSeconds;
    double accumulatedAbsoluteSkewSeconds;
} PublisherContext;

typedef struct {
    uint64_t callbackIndex;

    double adcTime;
    double adcDelta;

    double paCurrentTime;
    double paCurrentDelta;

    double linuxTime;
    double linuxDelta;

    double paInputAge;
    double clockOffsetChange;

    uint64_t estimatedMissingFrames;
    PaStreamCallbackFlags statusFlags;
    uint32_t eventReasons;
} TimingEvent;

typedef struct {
    SampleQueue *queue;
    float chScale[CHANNELS];
    const char *name;
    int printedRTProp;
    uint64_t nextSampleSequence;

    int targetCpu;
    _Atomic int affinityState;
    _Atomic int callbackTid;
    _Atomic int callbackCpu;
    _Atomic int affinityError;

    _Atomic uint64_t callbackCount;
    _Atomic uint64_t inputOverflowCount;
    _Atomic uint64_t otherStatusCount;
    _Atomic uint64_t nullInputCount;

    int timingInitialized;

    double previousAdcTime;
    double previousPaCurrentTime;
    double previousLinuxTime;

    double initialClockOffset;

    double minimumAdcDelta;
    double maximumAdcDelta;
    double maximumPaCurrentDelta;
    double maximumLinuxDelta;
    double maximumPaInputAge;
    double maximumClockOffsetChange;

    uint64_t adcDiscontinuityCount;
    uint64_t estimatedMissingFrames;

    uint64_t paDelayOver2ms;
    uint64_t paDelayOver3ms;
    uint64_t paDelayOver4ms;
    uint64_t paDelayOver5ms;
    uint64_t paDelayOver10ms;

    uint64_t linuxDelayOver2ms;
    uint64_t linuxDelayOver3ms;
    uint64_t linuxDelayOver4ms;
    uint64_t linuxDelayOver5ms;
    uint64_t linuxDelayOver10ms;

    TimingEvent *timingEvents;
    size_t timingEventCapacity;
    size_t timingEventCount;
    uint64_t discardedTimingEvents;
} StreamContext;

static bool SAMPLE_QUEUE_PUSH(SampleQueue *queue, const QueuedSample *sample);

static bool SAMPLE_QUEUE_POP(SampleQueue *queue, QueuedSample *sample);

static bool SAMPLE_QUEUE_EMPTY(const SampleQueue *queue);

static void ENQUEUE_DATA(
    const int16_t *samples,
    unsigned long frameCount,
    const PaStreamCallbackTimeInfo *timeInfo,
    StreamContext *ctx
);

static void PUBLISH_COMPLETE_FRAME(
    PublisherContext *publisher,
    const QueuedSample *sc0Sample,
    const QueuedSample *sc1Sample
);

static void *PUBLISHER_THREAD(void *argument);

static int CALLBACK(const void *inputBuffer, 
                    void *outputBuffer, 
                    unsigned long framesPerBuffer, 
                    const PaStreamCallbackTimeInfo* timeInfo, 
                    PaStreamCallbackFlags statusFlags, 
                    void *userData);

int FIND_DEVICE(const char *target_name);

PaStream* START_STREAM(char *targetDevice, StreamContext *ctx);

void PRINT_RT_PROPERTIES(StreamContext *ctx);

void CLEAN_UP(PaStream *stream0, PaStream *stream1);

static void HANDLE_SIGNAL(int signo);

static void RECORD_TIMING(StreamContext *ctx,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags);

static int WRITE_TIMING_FILE(const char *filename, const StreamContext *ctx);

static int WRITE_TIMING_SUMMARY_FILE(const char *filename, 
                                     const StreamContext *ctx,
                                     double durationSeconds);

static void PRINT_TIMING_SUMMARY(const StreamContext *ctx, double durationSeconds);

static void SET_CALLBACK_AFFINITY(StreamContext *ctx);

static int SET_CURRENT_THREAD_CPU(int cpu);

static void PRINT_CALLBACK_AFFINITY(const StreamContext *ctx);



#endif // PAREAD_H