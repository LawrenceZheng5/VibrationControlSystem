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

#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"

void PROCESS_DATA(const int16_t *samples, unsigned long frameCount);

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

int FIND_DEVICE(const char *target_name);

PaStream* START_STREAM(char *targetDevice);

#endif // PAREAD_H