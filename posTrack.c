/*
 * Modified readout to convert acceleration into position estimate
 * Implements double integral method to estimate position
 * Exponential moving average for bias correction
 * Press any button to exit the program read
 */

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

#define ACCEL1_CALIBRATION 1.042 // V/m/s2
#define ACCEL2_CALIBRATION 1.03
#define WINDOW_SAMPLES 400

#define DEBUG_MARKER(img)                        \
    do {                                         \
	(img)->md[0].write = 1;                  \
        (img)->array.SI32[0] = __LINE__;	 \
        (img)->md[0].write = 0;                  \
        ImageStreamIO_sempost((img), -1);        \
        (img)->md[0].cnt0++;                     \
    } while(0)

// Global Vars
//IMAGE *linarray; // Debug
IMAGE *sigarray;

// Raw ADC to acceleration: 
// (V_range / max_ADC)(1 / V_per_mps2)
float scaleAccel1 = 10.f / (32767.f * ACCEL1_CALIBRATION);
float scaleAccel2 = 10.f / (32767.f * ACCEL2_CALIBRATION);
static const double t = 1.0 / SAMPLE_RATE;

// Declare velocity position
static double vel1 = 0, vel2 = 0;
static double pos1 = 0, pos2 = 0;

// Set bias variables
static double bias1 = 0.0, bias2 = 0.0;
static const double BETA_BIAS  = 1e-5;          
static const double ALPHA_BIAS = 1.0 - BETA_BIAS;
float maxPos = 0;
float minPos = 0;
float maxA = 0;
float minA = 0;
static double total1 = 0, total2 = 0;

static FILE  *logfp   = NULL;
static uint64_t sampleIdx = 0;     


void PROCESS_DATA(const int16_t *samples, unsigned long frameCount);

static int CALLBACK(const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

int main() { 
  
  // Lock memory to only RAM 
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall");
    return 1;
  }

  // Create shm img
  int NBIMAGES = 1;
  long naxis;
  uint16_t atype;
  uint32_t size[2];
  int shared;
  int NBkw;

  sigarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  naxis = 2;
  size[0] = 6;
  size[1] = 1;
  atype = _DATATYPE_FLOAT;
  shared = 1;
  NBkw = 0;

  ImageStreamIO_createIm(&sigarray[0], "sig00", naxis, size, atype, shared, NBkw, 6);


  // Debugging shm img
  /*
  uint32_t sizeL[1];
  sizeL[0] = 2;
  linarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&linarray[0], "lin00", 1, sizeL, _DATATYPE_INT32, 1, 0, 6);
  */


  // Init PortAudio
  PaError err;
  PaStream *stream;
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
  
  // Log to CSV file
  logfp = fopen("accel_run14.csv", "w");
  if(!logfp) { perror("fopen"); return 1; }
  fprintf(logfp, "t,a1,a2,vel1,vel2,pos1,pos2\n");


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
    if (getchar()) break;
  }
  
  if (logfp) fclose(logfp);
  printf("MaxPos: %f\n", maxPos);
  printf("MinPos: %f\n", minPos);
  printf("Bias %f\n", bias1);
  printf("maxAccel %f\n", maxA);
  printf("minAccel %f\n", minA);

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
  sigarray->md[0].write = 1;
  float *buf = sigarray->array.F;


  for (unsigned long i = 0; i < frameCount; ++i) {
    // CH1
    double a1_raw = ((float)samples[i * 2] * scaleAccel1); // To ms/s2
    // CH2
    double a2_raw = ((float)samples[i * 2 + 1] * scaleAccel2);

    // Exponential moving average for bias
    bias1 = ALPHA_BIAS*bias1 + BETA_BIAS*a1_raw;
    bias2 = ALPHA_BIAS*bias2 + BETA_BIAS*a2_raw;
    
    float a1 = a1_raw - bias1;
    float a2 = a2_raw - bias2;

    // incremental double integration 
    float velDecay = 0.99999;
    float posDecay = 0.99999999;
    vel1 += a1 * t;
    vel1 *= velDecay;
    pos1 += vel1 * t + 0.5 * a1 * t * t;
    pos1 *= posDecay;

    vel2 += a2 * t;
    vel2 *= velDecay;
    pos2 += vel2 * t + 0.5 * a2 * t * t;
    pos2 *= posDecay;
   
    // Data Logging & Debugging 
    double t_now = sampleIdx * t;
    sampleIdx++;
    
    if (logfp) {
      fprintf(logfp, "%.6f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n", t_now, a1, a2, vel1, vel2, pos1, pos2);
    }
    if (pos2 > maxPos){
      maxPos = pos2;
    }
    if (pos2 < minPos){
      minPos = pos2;
    }
    if (a2_raw > maxA) maxA = a2_raw;
    if (a2_raw < minA) minA = a2_raw;
        
    // write to shared memory
        
    buf[0] = (float)(total1 + pos1);
    buf[1] = (float)(total2 + pos2);
    buf[2] = (float)vel1;
    buf[3] = (float)vel2;
    buf[4] = (float)a1;
    buf[5] = (float)a2;
  }

  // Write to shm
  sigarray[0].md[0].write = 0;
  ImageStreamIO_sempost(&sigarray[0], -1);
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
  
  const int16_t *samples = (const int16_t *)inputBuffer;
  PROCESS_DATA(samples, framesPerBuffer);
  return paContinue;
}
  
