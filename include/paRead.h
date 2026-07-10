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

#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"
#include "common.h"


#define CHANNELS 2


static void HANDLE_SIGNAL(int signo);

typedef struct {
    IMAGE *img;                     // Which milk SHM stream to write to
    float chScale[CHANNELS];        // Calibration scale factors
    const char *name;               // Debug name: "SC0", "SC1"
    int printedRTProp;              // Per-callback debug print
    int conditionerIndex;           // Index of the signal conditioner (0 or 1)
    uint64_t callbackCount;         // Count of how many times the callback has been called
    uint64_t inputOverflowCount;    // Count of input overflows
    uint64_t primingOutputCount;    // Count of priming outputs
    uint64_t otherStatusCount;      // Count of other statuses
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

#endif // PAREAD_H