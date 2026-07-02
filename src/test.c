/*
This is a test program for running code to test functions and API calls of 
libraries used in the Vibration Control System project. 
It is not part of the main program and is only used for testing purposes.
*/ 

#include <stdio.h>
#include <stdlib.h>
#include <portaudio.h>
#include <string.h>
#include <stdint.h>

// #define TARGET_NAME "485B39 200343308027880808317260" // Name of device on arecord
#define TARGET_NAME "USB Audio" // Name of device on arecord

#define CHANNELS 1
#define SAMPLE_RATE 8000
#define SAMPLE_FORMAT paInt16
#define FRAMES_PER_BUFFER 1

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData) {
/*
 * Callback function for PortAudio data
 */
  (void) outputBuffer;
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;
  
  if (inputBuffer == NULL) return paContinue;
  
  const int16_t *samples = (const int16_t *)inputBuffer;   
  printf("Received %lu samples: ", framesPerBuffer);
  for (unsigned long i = 0; i < framesPerBuffer; ++i) {
    printf("%d ", samples[i]);
    // fflush(stdout);
  }
  printf("\n");
//   PROCESS_DATA(samples, framesPerBuffer);

  return paContinue;
}

int main() {
    PaError err;
    PaStream *stream;
  
    // Init PortAudio
    err = Pa_Initialize();
    printf("\n");
    if (err != paNoError) {
        fprintf(stderr, "PortAudio Init Error: %s\n", Pa_GetErrorText(err));
        return 1;
    }
    
    // Find USB device
    int numDevices = Pa_GetDeviceCount();
    int targetDeviceIndex = -1;
    
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        printf("Info [Index: %d]: %s\n", i, info->name);
        printf("Max Input Channels: %d\n", info->maxInputChannels);
        printf("Default Sample Rate: %.2f\n", info->defaultSampleRate);
        printf("Host API: %s\n", Pa_GetHostApiInfo(info->hostApi)->name);
        printf("Default Low Input Latency: %.6f seconds\n", info->defaultLowInputLatency);
        printf("Default High Input Latency: %.6f seconds\n", info->defaultHighInputLatency);
        printf("Struct version: %d\n", info->structVersion);
        printf("\n");
        // if (info->maxInputChannels > 0 && strstr(info->name, TARGET_NAME)) {
        //     printf("Found device [%d]: %s\n", i, info->name);
        //     targetDeviceIndex = i;
        //     break;
        // }
    }
    PaStreamParameters inputParams;
    inputParams.device = targetDeviceIndex;
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
    printf("host api index %d\n", Pa_GetDefaultHostApi());
    while(1) {
        Pa_Sleep(1); // Sleep for 1 second
    }
    Pa_Terminate();
    return 0;
}