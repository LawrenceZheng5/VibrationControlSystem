#include <stdio.h>
#include <stdlib.h>
#include <portaudio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"

#define SAMPLE_RATE 8000
#define SAMPLE_FORMAT paInt16
#define FRAMES_PER_BUFFER 1
#define CHANNELS 2
#define TARGET_NAME "USB Audio" // Name of device on arecord 

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


void PROCESS_DATA(const int16_t *samples, unsigned long frameCount) {
/*
 * Helper function for callback
 * Turns into acceleration
 * Writes to milk shm
 */  
  sigarray->md[0].write = 1;
  float *buf = sigarray->array.F;
  for (unsigned long i = 0; i < frameCount; ++i) {
    // CH1
    buf[i * 2] = (((float)samples[i * 2])/32767) * (10/ACCEL1_CALIBRATION); // To ms/s2
    // CH2
    buf[i * 2 + 1] = (((float)samples[i * 2 + 1])/32767) * (10/ACCEL2_CALIBRATION); // To m/s2
    
  }
  // Write to shm
  sigarray[0].md[0].write = 0;
  ImageStreamIO_sempost(&sigarray[0], -1);
  sigarray[0].md[0].cnt0++;
  sigarray[0].md[0].cnt1++;

}

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
/*
 * Callback function for PortAudio data
 */
{
	
  (void) outputBuffer;
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;
  

  if (inputBuffer == NULL) return paContinue;
  
  const int16_t *samples = (const int16_t *)inputBuffer;
  PROCESS_DATA(samples, framesPerBuffer);
  return paContinue;
}

int main() {
  
  // Lock memory to only RAM 
  mlockall(MCL_CURRENT | MCL_FUTURE);
  
  // Create shm img
  int NBIMAGES = 1;
  long naxis;
  uint16_t atype;
  uint32_t size[2];
  int shared;
  int NBkw;

  sigarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  naxis = 2;
  size[0] = 2;
  size[1] = 1;
  atype = _DATATYPE_FLOAT;
  shared = 1;
  NBkw = 0;

  ImageStreamIO_createIm(&sigarray[0], "sig00", naxis, size, atype, shared, NBkw, 6);


  // Debugging shm img
  uint32_t sizeL[1];
  sizeL[0] = 2;
  linarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&linarray[0], "lin00", 1, sizeL, _DATATYPE_INT32, 1, 0, 6);
  
  PaError err;
  PaStream *stream;
  
  // Init PortAudio
  err = Pa_Initialize();
  if (err != paNoError) {
    fprintf(stderr, "PortAudio Init Error: %s\n", Pa_GetErrorText(err));
    return 1;
  }
  
  // Find USB device
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
    Pa_Sleep(1);
  }
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  return 0;
}




