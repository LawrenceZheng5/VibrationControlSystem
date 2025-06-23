#include <stdio.h>
#include <stdlib.h>
#include <portaudio.h>
#include <stdint.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define SAMPLE_FORMAT paInt16
#define FRAMES_PER_BUFFER paFramesPerBufferUnspecified
#define CHANNELS 2
#define TARGET_NAME "cx231xx"

void PROCESS_DATA(const int16_t *samples, unsigned long frameCount) {
  printf("Left\t\tRight\n");
  
  for (unsigned long i = 0; i < frameCount; ++i) {
    int16_t left = samples[i * 2];
    int16_t right = samples[i * 2 + 1];
    printf("%6d\t\t%6d\n", left, right);
  }
  
  printf("<--------------------------->\n");
}

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
{
  (void) outputBuffer;
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;
  
  if (inputBuffer == NULL) return paContinue;
  
  const int16_t *samples = (const int16_t *)inputBuffer;
  PROCESS_DATA(samples, framesPerBuffer);
  
  double callbackTime = timeInfo->currentTime;
  double inputTime = timeInfo->inputBufferAdcTime;
  
  double latencyEst = callbackTime - inputTime;
  
  printf("Est latency: %f\n", latencyEst);
  printf("FPB: %d\n", paFramesPerBufferUnspecified);
  
  return paContinue;
}

int main() {
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
  
  printf("Reading device %s", TARGET_NAME);
  
  const PaStreamInfo *streamInfo = Pa_GetStreamInfo(stream);
  if (streamInfo != NULL) {
    printf("Input Latency: %.6f seconds\n", streamInfo->inputLatency);
  }
  else {
    fprintf(stderr, "Failed to report latency");
  }
  
  while(1){
    Pa_Sleep(100);
  }
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  return 0;
}




