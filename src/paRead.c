#define _GNU_SOURCE

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

#define SAMPLE_RATE 8000
#define SAMPLE_FORMAT paInt16
#define FRAMES_PER_BUFFER 1
#define CHANNELS 2
#define TARGET_NAME "485B39 200288708050190807212250" // Serial number of SC0

// Change this when connecting accelerometer with different calibration 
#define ACCEL1_CALIBRATION 1.042 // V/m/s^2
#define ACCEL2_CALIBRATION 1.03

#define DEBUG_MARKER(img)                        \
    do {                                         \
	(img)->md[0].write = 1;                  \
        (img)->array.SI32[0] = __LINE__;	 \
        (img)->md[0].write = 0;                  \
        ImageStreamIO_sempost((img), -1);        \
        (img)->md[0].cnt0++;                     \
    } while(0)

// Global Vars
IMAGE *linarray;
IMAGE *sigarray;
// Raw to acceleration conversions
float scaleAccel1 = 10.f / (32767.f * ACCEL1_CALIBRATION);
float scaleAccel2 = 10.f / (32767.f * ACCEL2_CALIBRATION);

int printedRTProp = 0;

void PROCESS_DATA(const int16_t *samples, unsigned long frameCount);

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

int main() {
  
  // Lock memory to only RAM 
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall");
    return 1;
  }

  // Create shm img
  uint32_t size[2];
  int NBIMAGES   = 1;
  long naxis     = 2;
  size[0]        = CHANNELS;          // 2 channels
  size[1]        = FRAMES_PER_BUFFER; // 1 sample per frame
  uint16_t atype = _DATATYPE_FLOAT;
  int shared     = 1 ;
  int NBkw       = 0;
  int CBsize     = 6;                 // Circular buffer size


  sigarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&sigarray[0], "sig00", naxis, size, atype, shared, NBkw, CBsize);


  // Debugging shm img
  uint32_t sizeL[1];
  sizeL[0] = 2;
  linarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&linarray[0], "lin00", 1, sizeL, _DATATYPE_INT32, 1, 0, CBsize);

  PaError err;
  PaStream *stream;
  
  // Init PortAudio
  err = Pa_Initialize();
  if (err != paNoError) {
    fprintf(stderr, "PortAudio Init Error: %s\n", Pa_GetErrorText(err));
    return 1;
  }
  
  // Find Signal Conditioner 
  int numDevices = Pa_GetDeviceCount();
  int targetDevice = -1;
  
  for (int i = 0; i < numDevices; i++) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
    if (info->maxInputChannels > 0 && strstr(info->name, TARGET_NAME)) {
      printf("Found device [%d]: %s\n", i, info->name);
      targetDevice = i;
      break;
    }
  }
  
  if (targetDevice == -1) {
    fprintf(stderr, "No matching input found for %s\n", TARGET_NAME);
    Pa_Terminate();
    return 1;
  }

  // Set PortAudio params & open stream
  PaStreamParameters inputParams;
  inputParams.device = targetDevice;
  inputParams.channelCount = CHANNELS;
  inputParams.sampleFormat = SAMPLE_FORMAT;
  inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
  inputParams.hostApiSpecificStreamInfo = NULL;
 
  err = Pa_OpenStream(&stream, &inputParams, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, CALLBACK, NULL);
 
  if (err != paNoError){
    fprintf(stderr, "Pa_OpenStream error: %s\n", Pa_GetErrorText(err));
    Pa_Terminate();
    return 1;
    }
  
  err = Pa_StartStream(stream);

  if (err != paNoError){
    fprintf(stderr, "Pa_StartStream error: %s\n", Pa_GetErrorText(err));
    Pa_Terminate();
    return 1;
  }

  printf("Reading device %s\n", TARGET_NAME);
  
  const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream);
  if (streamInfo != NULL) {
    printf("Input Latency: %.6f seconds\n", streamInfo->inputLatency);
  }
  else {
    fprintf(stderr, "Failed to report latency");
  }  
  while(1){
    Pa_Sleep(1000);
  }
  munlockall();
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  
  return 0;
}


void PROCESS_DATA(const int16_t *samples, unsigned long frameCount) {
/*
 * Helper function for callback
 * Turns into acceleration
 * Writes to milk shm
 */  

  // Writing 1 to indicate writing started
  sigarray->md[0].write = 1;

  // Indicate the type of data (same as earlier defined)
  float *buf = sigarray->array.F;

  // printf("Processing %lu samples\n", frameCount);

  // Write data
  for (unsigned long i = 0; i < frameCount; ++i) {
    // samples[frame_index * CHANNELS + channel_index]

    // CH1
    buf[i * CHANNELS + 0] = ((float)samples[i * 2] * scaleAccel1); // To ms/s2
    // printf("sample[%lu]: CH1: %d, CH2: %d -> Accel1: %.6f m/s^2, Accel2: %.6f m/s^2\n", i, samples[i * 2], samples[i * 2 + 1], buf[i * 2], buf[i * 2 + 1]);
    
    // CH2
    buf[i * CHANNELS + 1] = ((float)samples[i * 2 + 1] * scaleAccel2); // To m/s2
    // printf("sample[%lu]: CH1: %d, CH2: %d -> Accel1: %.6f m/s^2, Accel2: %.6f m/s^2\n", i, samples[i * 2], samples[i * 2 + 1], buf[i * 2], buf[i * 2 + 1]);
  }

  // Write 0 to indicate writing finished
  sigarray[0].md[0].write = 0;

  // Post semaphore to indicate downstream that data is ready
  ImageStreamIO_sempost(&sigarray[0], -1);

  // Increment counters to indicate new data is available
  sigarray[0].md[0].cnt0++;
  sigarray[0].md[0].cnt1++;

}

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData) {
/*
 * Callback function for PortAudio data
 */

	
  (void) outputBuffer;
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;
  
  if (inputBuffer == NULL) return paContinue;
  if (!printedRTProp) {
    printedRTProp = 1;

    // Print RT properties of the callback thread
    pid_t tid = syscall(SYS_gettid);

    int policy = sched_getscheduler(0);
    struct sched_param param;
    sched_getparam(0, &param);

    printf("Callback running on TID %d\n", tid);
    printf("Scheduler policy: %d\n", policy);
    printf("RT priority: %d\n", param.sched_priority);
    fflush(stdout);
  }

  const int16_t *samples = (const int16_t *)inputBuffer;
  PROCESS_DATA(samples, framesPerBuffer);
  return paContinue;
}
  
