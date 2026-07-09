#define _GNU_SOURCE

#include "paRead.h"

#define NUM_SC 2
#define SAMPLE_RATE 8000
#define SAMPLE_FORMAT paInt16
#define FRAMES_PER_BUFFER 1

// Serial Numbers for the two signal conditioners
#define SC0 "485B39 200288708050190807212250" 
#define SC1 "485B39 200343308027880808317260" 

// Change this when connecting accelerometer with different calibration 
// Only using 3 accel for 3 axis on one of those aluminum mounting block
#define SC0_CH1_ACCEL_CALIBRATION 1.042 // V/m/s^2
#define SC0_CH2_ACCEL_CALIBRATION 1.03
#define SC1_CH1_ACCEL_CALIBRATION 1.034

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
IMAGE *sigarray0;

static volatile sig_atomic_t keepRunning = 1;


int main() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = HANDLE_SIGNAL;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, NULL) != 0) {
    perror("sigaction SIGINT");
    return 1;
  }

  if (sigaction(SIGTERM, &sa, NULL) != 0) {
    perror("sigaction SIGTERM");
    return 1;
  }

  if (sigaction(SIGQUIT, &sa, NULL) != 0) {
    perror("sigaction SIGQUIT");
    return 1;
  }


  // Lock memory to only RAM 
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall");
    return 1;
  }

  // Create shm img
  uint32_t size[3];
  int NBIMAGES   = 1;
  long naxis     = 3;
  size[0]        = CHANNELS;           // 2 Channels per Conditioner
  size[1]        = NUM_SC;             // 2 Signal Conditioners
  size[2]        = FRAMES_PER_BUFFER;  // 1 Frame per Buffer
  uint16_t atype = _DATATYPE_FLOAT;
  int shared     = 1 ;
  int NBkw       = 0;
  int CBsize     = 6;                 // Circular buffer size

  // For Accels on SC0
  sigarray0 = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&sigarray0[0], "sig00", naxis, size, atype, shared, NBkw, CBsize);

  StreamContext ctx0 = {
    .img = &sigarray0[0],
    .chScale = {
      10.f / (32767.f * SC0_CH1_ACCEL_CALIBRATION),
      10.f / (32767.f * SC0_CH2_ACCEL_CALIBRATION)
    },
    .name = "SC0",
    .printedRTProp = 0,
    .conditionerIndex = 0
  };

  StreamContext ctx1 = {
      .img = &sigarray0[0],
      .chScale = {
        10.f / (32767.f * SC1_CH1_ACCEL_CALIBRATION),
        0.0f  // unused or set this when connecting a 4th accelerometer
      },
      .name = "SC1",
      .printedRTProp = 0,
      .conditionerIndex = 1
  };

  // Debugging shm img
  uint32_t sizeL[1];
  sizeL[0] = 2;
  linarray = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&linarray[0], "lin00", 1, sizeL, _DATATYPE_INT32, 1, 0, CBsize);

  PaError err;

  // Init PortAudio
  err = Pa_Initialize();
  if (err != paNoError) {
    fprintf(stderr, "PortAudio Init Error: %s\n", Pa_GetErrorText(err));
    return 1;
  }
  

  PaStream* stream0 = START_STREAM(SC0, &ctx0);
  PaStream* stream1 = START_STREAM(SC1, &ctx1);
  
  // const PaStreamInfo *streamInfo0 = Pa_GetStreamInfo(stream0);
  // const PaStreamInfo *streamInfo1 = Pa_GetStreamInfo(stream1);

  // if (streamInfo0 != NULL && streamInfo1 != NULL) {
  //   printf("Input Latency: %.6f seconds\n", streamInfo0->inputLatency);
  //   printf("Input Latency: %.6f seconds\n", streamInfo1->inputLatency);
  // }
  // else {
  //   fprintf(stderr, "Failed to report latency");
  // }  

  PRINT_RT_PROPERTIES(NULL);


  while (keepRunning) {
    Pa_Sleep(1000);
  }

  printf("\nStopping acquisition...\n");
  CLEAN_UP(stream0, stream1);
  printf("Cleanup complete.\n");

  return 0;
}

static void HANDLE_SIGNAL(int signo) {
  (void) signo;
  keepRunning = 0;
}

void PROCESS_DATA(const int16_t *samples, unsigned long frameCount, StreamContext *ctx) {
/*
 * Helper function for callback
 * Turns into acceleration
 * Writes to milk shm
 */  

  IMAGE *img = ctx->img;

  // Indicate the type of data (same as earlier defined)
  float *buf = img->array.F;
  
  int conditionerIndex = ctx->conditionerIndex;

  // Writing 1 to indicate writing started
  img->md[0].write = 1;

  // printf("Processing %lu samples\n", frameCount);

  // Write data
  for (unsigned long frames = 0; frames < frameCount; ++frames) {
    for (int ch = 0; ch < CHANNELS; ++ch) {
      unsigned long index = ch + CHANNELS * conditionerIndex + CHANNELS * NUM_SC * frames;

      buf[index] = ((float)samples[frames * CHANNELS + ch] * ctx->chScale[ch]); // To m/s^2
    }
  }

/*
  * cnt0 = normal frame/update counter
  * cnt1 = tag showing which signal conditioner wrote most recently
  *
  * cnt1 = 0 means SC0
  * cnt1 = 1 means SC1
  */
  img->md[0].cnt0++;
  img->md[0].cnt1 = conditionerIndex; // Indicate which conditioner the data is from

  // Write 0 to indicate writing finished
  img->md[0].write = 0;

  /*
  * Priority mode:
  * Only SC0 posts the semaphore.
  * SC1 updates its slice, but does not wake up downstream consumers.
  */
  ImageStreamIO_sempost(img, -1);

  // printf("Stream:%s On Ch1 Count %ld, Ch2 Count %ld\n", ctx->name, img->md[0].cnt0, img->md[0].cnt1);
}

static int CALLBACK(const void *inputBuffer, 
                    void *outputBuffer, 
                    unsigned long framesPerBuffer, 
                    const PaStreamCallbackTimeInfo* timeInfo, 
                    PaStreamCallbackFlags statusFlags, 
                    void *userData
                  ) {
/*
 * Callback function for PortAudio data
 */

	
  (void) outputBuffer;
  (void) timeInfo;
  (void) statusFlags;
  
  StreamContext *ctx = (StreamContext *)userData;
  
  if (inputBuffer == NULL) return paContinue;

  if (!ctx->printedRTProp) {
    ctx->printedRTProp = 1;

    PRINT_RT_PROPERTIES(ctx);

  }

  const int16_t *samples = (const int16_t *)inputBuffer;
  PROCESS_DATA(samples, framesPerBuffer, (StreamContext *)userData);
  return paContinue;
}

int FIND_DEVICE(const char *target_name) {
  int numDevices = Pa_GetDeviceCount();
  int targetDevice = -1;
  
  for (int i = 0; i < numDevices; i++) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
    if (info->maxInputChannels > 0 && strstr(info->name, target_name)) {
      printf("Found device [%d]: %s\n", i, info->name);
      targetDevice = i;
      break;
    }
  }
  
  if (targetDevice == -1) {
    fprintf(stderr, "No matching input found for %s\n", target_name);
    return -1;
  }
  
  return targetDevice;
}

PaStream* START_STREAM(char *targetDevice, StreamContext *ctx) {
  PaError err;
  PaStream *stream;

  int device = FIND_DEVICE(targetDevice);
  if (device < 0) {
    return NULL;
  }

  // Set PortAudio params & open stream
  PaStreamParameters inputParams;
  inputParams.device = device;
  inputParams.channelCount = CHANNELS;
  inputParams.sampleFormat = SAMPLE_FORMAT;
  inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
  inputParams.hostApiSpecificStreamInfo = NULL;

  err = Pa_OpenStream(&stream,
                      &inputParams, 
                      NULL, 
                      SAMPLE_RATE, 
                      FRAMES_PER_BUFFER, 
                      paClipOff, 
                      CALLBACK, 
                      ctx);

  if (err != paNoError){
    fprintf(stderr, "Pa_OpenStream error: %s\n", Pa_GetErrorText(err));
    return NULL;
  }

  err = Pa_StartStream(stream);
  
  if (err != paNoError){
    fprintf(stderr, "Pa_StartStream error: %s\n", Pa_GetErrorText(err));
    return NULL;
  }

  return stream;
}

void PRINT_RT_PROPERTIES(StreamContext *ctx) {
  
  // Print RT properties of the callback thread
  pid_t tid = syscall(SYS_gettid);

  int policy = sched_getscheduler(0);
  struct sched_param param;
  sched_getparam(0, &param);

  
  int cpu = sched_getcpu();
  
  printf("\n");
  if (ctx != NULL) {
      printf("---- PortAudio Callback RT Properties for %s ----\n", ctx->name);
  } else {
      printf("---- Main RT Properties ----\n");
  }
  printf("Callback running on TID %d\n", tid);
  printf("Running on CPU %d\n", cpu);
  printf("Scheduler policy: %d\n", policy);
  printf("RT priority: %d\n", param.sched_priority);
  fflush(stdout);
}

void CLEAN_UP (PaStream *stream0, PaStream *stream1) {
  if (stream0 != NULL) {
    Pa_StopStream(stream0);
    Pa_CloseStream(stream0);
  }

  if (stream1 != NULL) {
    Pa_StopStream(stream1);
    Pa_CloseStream(stream1);
  }

  Pa_Terminate();

  if (sigarray0 != NULL) {
    ImageStreamIO_destroyIm(&sigarray0[0]);
    free(sigarray0);
    sigarray0 = NULL;
  }


  if (linarray != NULL) {
    ImageStreamIO_destroyIm(&linarray[0]);
    free(linarray);
    linarray = NULL;
  }

  munlockall();
}