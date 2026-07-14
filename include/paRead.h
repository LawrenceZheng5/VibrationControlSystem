#ifndef PAREAD_H
#define PAREAD_H

#include <stdio.h>
#include <stdlib.h>
#include <portaudio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <math.h>
#include <float.h>
#include <inttypes.h>

#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"
#include "helper.h"


#define CHANNELS 2
#define MAX_TIMING_EVENTS 65536

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
} TimingEvent;

typedef struct {
    IMAGE *img;
    float chScale[CHANNELS];
    const char *name;
    int printedRTProp;
    int conditionerIndex;

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

    TimingEvent *timingEvents;
    size_t timingEventCapacity;
    size_t timingEventCount;
    uint64_t discardedTimingEvents;
} StreamContext;

void PROCESS_DATA(const int16_t *samples, unsigned long frameCount, StreamContext *ctx);

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

static void PRINT_TIMING_SUMMARY(const StreamContext *ctx);

static double per_million(uint64_t count, uint64_t callbacks);


#endif // PAREAD_H