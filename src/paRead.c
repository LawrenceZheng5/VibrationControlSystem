#define _GNU_SOURCE

#include "paRead.h"


#define NUM_SC 2
#define SAMPLE_RATE 8000.00
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

// Define thresholds for timing events
#define SEVERE_PA_DELAY_S         0.004
#define SEVERE_LINUX_DELAY_S      0.004
#define TIMING_EVENT_ADC_JUMP     (1u << 0)
#define TIMING_EVENT_PA_DELAY     (1u << 1)
#define TIMING_EVENT_LINUX_DELAY  (1u << 2)
#define TIMING_EVENT_PA_STATUS    (1u << 3)

// CPU affinity for the main thread, signal conditioner 0, signal conditioner 1, and USB IRQ handler
#define MAIN_CPU    8
#define SC0_CPU     10
#define USB_IRQ_CPU 12
#define SC1_CPU     14

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

static SampleQueue sc0Queue;
static SampleQueue sc1Queue;

static volatile sig_atomic_t keepRunning = 1;


int main(int argc, char *argv[]) {
  // Handle signals for graceful termination
  // SIGINT (Ctrl+C), SIGTERM, and SIGQUIT 
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

  // Hande SIGPIPE to avoid termination when writing to a closed pipe/socket 
  // ie for when using in conjunction with tee to save conole output to file
  struct sigaction ignorePipe;
  memset(&ignorePipe, 0, sizeof(ignorePipe));
  ignorePipe.sa_handler = SIG_IGN;
  sigemptyset(&ignorePipe.sa_mask);

  if (sigaction(SIGPIPE, &ignorePipe, NULL) != 0) {
      perror("sigaction SIGPIPE");
      return 1;
  }


  // Lock memory to only RAM 
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    perror("mlockall");
    return 1;
  }

  char *outputDirectory = GET_OUTPUT_DIRECTORY(argc, argv);

  if (outputDirectory == NULL) {
    return 1;
  }

  printf("Timing files will be saved to:\n%s\n", outputDirectory);

  char sc0TimingCsvPath[PATH_MAX];
  char sc0SummaryCsvPath[PATH_MAX];
  char sc1TimingCsvPath[PATH_MAX];
  char sc1SummaryCsvPath[PATH_MAX];

  if (BUILD_FILE_PATH(sc0TimingCsvPath, sizeof(sc0TimingCsvPath),
                      outputDirectory, "sc0_timing.csv") != 0 ||
      BUILD_FILE_PATH(sc0SummaryCsvPath, sizeof(sc0SummaryCsvPath),
                      outputDirectory, "sc0_timing_summary.csv") != 0 || 
      BUILD_FILE_PATH(sc1TimingCsvPath, sizeof(sc1TimingCsvPath),
                      outputDirectory, "sc1_timing.csv") != 0 ||
      BUILD_FILE_PATH(sc1SummaryCsvPath, sizeof(sc1SummaryCsvPath),
                      outputDirectory, "sc1_timing_summary.csv") != 0) {

      free(outputDirectory);
      fprintf(stderr, "Failed to build output file paths.\n");
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
  int CBsize     = 1;                 // Circular buffer size

  // For Accels on SC0
  sigarray0 = (IMAGE*) malloc(sizeof(IMAGE)*NBIMAGES);
  ImageStreamIO_createIm(&sigarray0[0], "accel", naxis, size, atype, shared, NBkw, CBsize);

  sigarray0[0].md[0].cnt1 = 0;

  StreamContext ctx0 = {0};

  ctx0.queue = &sc0Queue;
  ctx0.chScale[0] = 10.f / (32767.f * SC0_CH1_ACCEL_CALIBRATION);
  ctx0.chScale[1] = 10.f / (32767.f * SC0_CH2_ACCEL_CALIBRATION);
  ctx0.name = "SC0";
  ctx0.targetCpu = SC0_CPU;
  ctx0.timingEventCapacity = MAX_TIMING_EVENTS;

  StreamContext ctx1 = {0};

  ctx1.queue = &sc1Queue;
  ctx1.chScale[0] = 10.f / (32767.f * SC1_CH1_ACCEL_CALIBRATION);
  ctx1.chScale[1] = 0.0f;
  ctx1.name = "SC1";
  ctx1.targetCpu = SC1_CPU;
  ctx1.timingEventCapacity = MAX_TIMING_EVENTS;


  ctx0.timingEvents = calloc(ctx0.timingEventCapacity,sizeof(*ctx0.timingEvents));

  ctx1.timingEvents = calloc(ctx1.timingEventCapacity,sizeof(*ctx1.timingEvents));

  if (ctx0.timingEvents == NULL) {
      perror("calloc SC0 timing events");
      CLEAN_UP(NULL, NULL);
      free(outputDirectory);
      return 1;
  }

  if (ctx1.timingEvents == NULL) {
      perror("calloc SC1 timing events");
      free(ctx0.timingEvents);
      ctx0.timingEvents = NULL;
      CLEAN_UP(NULL, NULL);
      free(outputDirectory);
      return 1;
  }

  memset(ctx0.timingEvents, 0, ctx0.timingEventCapacity * sizeof(*ctx0.timingEvents));

  memset(ctx1.timingEvents, 0, ctx1.timingEventCapacity * sizeof(*ctx1.timingEvents)); 

  // if (SET_CURRENT_THREAD_CPU(MAIN_CPU) != 0) {
  //   perror("Failed to pin main thread");
  //   return 1;
  // }

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
  PublisherContext publisher = {0};

  publisher.img = &sigarray0[0];

  publisher.sc0Queue = &sc0Queue;
  publisher.sc1Queue = &sc1Queue;

  publisher.sc0Scale[0] = ctx0.chScale[0];
  publisher.sc0Scale[1] = ctx0.chScale[1];

  publisher.sc1Scale[0] = ctx1.chScale[0];
  publisher.sc1Scale[1] = ctx1.chScale[1];

  publisher.matchToleranceSeconds = 0.75 / SAMPLE_RATE;

  atomic_init(&publisher.stopRequested, 0);

  pthread_t publisherThread;

  const int publisherCreateError = pthread_create(
      &publisherThread,
      NULL,
      PUBLISHER_THREAD,
      &publisher
  );

  if (publisherCreateError != 0) {
    fprintf(stderr, "pthread_create publisher failed: %s\n", strerror(publisherCreateError));

    CLEAN_UP(NULL, NULL);
    free(outputDirectory);
    return 1;
  }

  PaStream* stream0 = START_STREAM(SC0, &ctx0);
  PaStream* stream1 = START_STREAM(SC1, &ctx1);

  if (stream0 == NULL || stream1 == NULL) {
    fprintf(stderr, "Failed to start both signal conditioners.\n");

    if (stream0 != NULL) {
      Pa_StopStream(stream0);
    }

    if (stream1 != NULL) {
      Pa_StopStream(stream1);
    }

    atomic_store_explicit(&publisher.stopRequested, 1, memory_order_release);

    pthread_join(publisherThread, NULL);

    CLEAN_UP(stream0, stream1);
    free(outputDirectory);
    return 1;
  }
  double acquisitionStartTime = now_sec();

  // for (int attempt = 0; attempt < 100; ++attempt) {
  //     int sc0State = atomic_load_explicit(&ctx0.affinityState, memory_order_acquire);
  //     int sc1State = atomic_load_explicit(&ctx1.affinityState, memory_order_acquire);

  //     if (sc0State != 0 && sc1State != 0) {
  //       break;
  //     }

  //     Pa_Sleep(10);
  // }

  // PRINT_CALLBACK_AFFINITY(&ctx0);
  // PRINT_CALLBACK_AFFINITY(&ctx1);


  printf("Acquisition started at: %f\n", acquisitionStartTime);

  PRINT_RT_PROPERTIES(NULL);

  while (keepRunning) {
    Pa_Sleep(1000);
  }

  printf("\nStopping acquisition...\n");
  printf("\nSummary:\n");
  fflush(stdout);

  if (stream0 != NULL) {
    PaError stopErr = Pa_StopStream(stream0);
    if (stopErr != paNoError) {
        fprintf(stderr, "SC0 Pa_StopStream error: %s\n", Pa_GetErrorText(stopErr));
        fflush(stderr);
        fflush(stdout);
    }
  }
    
  if (stream1 != NULL) {
    PaError stopErr = Pa_StopStream(stream1);

    if (stopErr != paNoError) {
      fprintf(stderr, "SC1 Pa_StopStream error: %s\n", Pa_GetErrorText(stopErr));
      fflush(stderr);
      fflush(stdout);
    }
  }
  atomic_store_explicit(
    &publisher.stopRequested,
    1,
    memory_order_release
  );

  const int publisherJoinError = pthread_join(publisherThread, NULL);

  if (publisherJoinError != 0) {
      fprintf(stderr, "pthread_join publisher failed: %s\n", strerror(publisherJoinError));
  }

  double acquistionDuration = now_sec() - acquisitionStartTime;

  PRINT_TIMING_SUMMARY(&ctx0, acquistionDuration);
  PRINT_TIMING_SUMMARY(&ctx1, now_sec() - acquisitionStartTime);
  const uint64_t sc0QueueDrops = atomic_load_explicit(&sc0Queue.fullDropCount, memory_order_relaxed);

  const uint64_t sc1QueueDrops = atomic_load_explicit(&sc1Queue.fullDropCount, memory_order_relaxed);

  const double meanSkewMicroseconds =
      publisher.publishedFrames > 0
          ? (
              publisher.accumulatedAbsoluteSkewSeconds /
              (double)publisher.publishedFrames
            ) * 1.0e6
          : 0.0;

  printf("\nPublisher summary\n");
  printf("Published complete frames:              %" PRIu64 "\n", publisher.publishedFrames);
  printf("SC0 queue-full drops:                   %" PRIu64 "\n", sc0QueueDrops);
  printf("SC1 queue-full drops:                   %" PRIu64 "\n", sc1QueueDrops);
  printf("SC0 unmatched timestamp samples:        %" PRIu64 "\n", publisher.unmatchedSc0);
  printf("SC1 unmatched timestamp samples:        %" PRIu64 "\n", publisher.unmatchedSc1);
  printf("Mean absolute SC0/SC1 skew:             %.3f us\n", meanSkewMicroseconds);
  printf("Maximum accepted SC0/SC1 skew:          %.3f us\n", publisher.maximumAbsoluteSkewSeconds * 1.0e6);
  fflush(stdout);

  printf("Writing timing files...\n");
  fflush(stdout);

  WRITE_TIMING_FILE(sc0TimingCsvPath, &ctx0);
  WRITE_TIMING_FILE(sc1TimingCsvPath, &ctx1);
  WRITE_TIMING_SUMMARY_FILE(sc0SummaryCsvPath, &ctx0, acquistionDuration);
  WRITE_TIMING_SUMMARY_FILE(sc1SummaryCsvPath, &ctx1, acquistionDuration);  
  
  printf("Timing files written.\n");
  fflush(stdout);
  
  printf("Starting cleanup...\n");
  fflush(stdout);

  free(ctx0.timingEvents);
  free(ctx1.timingEvents);

  free(outputDirectory);

  ctx0.timingEvents = NULL;
  ctx1.timingEvents = NULL;
  outputDirectory = NULL;
  
  fflush(stdout);
  fflush(stderr);

  CLEAN_UP(stream0, stream1);
  // CLEAN_UP(stream0, NULL);



  printf("Cleanup complete.\n");
  fflush(stdout);

  return 0;
}

static void HANDLE_SIGNAL(int signo) {
  (void) signo;
  keepRunning = 0;
}

static bool SAMPLE_QUEUE_PUSH(SampleQueue *queue, const QueuedSample *sample) {
  const uint64_t writeIndex = atomic_load_explicit(
      &queue->writeIndex,
      memory_order_relaxed
  );

  const uint64_t readIndex = atomic_load_explicit(
      &queue->readIndex,
      memory_order_acquire
  );

  /*
    * The queue is full when the producer is one complete
    * queue capacity ahead of the consumer.
    */
  if (writeIndex - readIndex >= SAMPLE_QUEUE_CAPACITY) {
    atomic_fetch_add_explicit(
        &queue->fullDropCount,
        1,
        memory_order_relaxed
    );

    return false;
  }

  queue->samples[writeIndex & SAMPLE_QUEUE_MASK] = *sample;

  /*
    * Release publishes the completed sample to the consumer.
    */
  atomic_store_explicit(
      &queue->writeIndex,
      writeIndex + 1,
      memory_order_release
  );

  return true;
}

static bool SAMPLE_QUEUE_POP(SampleQueue *queue, QueuedSample *sample) {
  const uint64_t readIndex = atomic_load_explicit(
      &queue->readIndex,
      memory_order_relaxed
  );

  const uint64_t writeIndex = atomic_load_explicit(
      &queue->writeIndex,
      memory_order_acquire
  );

  if (readIndex == writeIndex) {
    return false;
  }

  *sample = queue->samples[readIndex & SAMPLE_QUEUE_MASK];

  /*
    * Release allows the producer to reuse this queue slot.
    */
  atomic_store_explicit(
      &queue->readIndex,
      readIndex + 1,
      memory_order_release
  );

  return true;
}

static bool SAMPLE_QUEUE_EMPTY(const SampleQueue *queue) {
  const uint64_t readIndex = atomic_load_explicit(
      &queue->readIndex,
      memory_order_acquire
  );

  const uint64_t writeIndex = atomic_load_explicit(
      &queue->writeIndex,
      memory_order_acquire
  );

  return readIndex == writeIndex;
}

static void ENQUEUE_DATA(const int16_t *samples, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, StreamContext *ctx) {
    if (samples == NULL || timeInfo == NULL || ctx == NULL || ctx->queue == NULL) {
        return;
    }

    /*
     * inputBufferAdcTime corresponds to the first sample in
     * this callback buffer.
     */
    const double firstAdcTime = timeInfo->inputBufferAdcTime;

    for (unsigned long frame = 0; frame < frameCount; ++frame) {
        QueuedSample sample;

        sample.adcTime = firstAdcTime + (double)frame / SAMPLE_RATE;

        sample.sequence = ctx->nextSampleSequence++;

        sample.channel[0] = samples[frame * CHANNELS + 0];

        sample.channel[1] = samples[frame * CHANNELS + 1];

        /*
         * When the queue is full, SAMPLE_QUEUE_PUSH records
         * the drop. Do not block the PortAudio callback.
         */
        (void)SAMPLE_QUEUE_PUSH(
            ctx->queue,
            &sample
        );
    }
}

static void PUBLISH_COMPLETE_FRAME(PublisherContext *publisher, const QueuedSample *sc0Sample, const QueuedSample *sc1Sample) {
  IMAGE *img = publisher->img;
  float *buffer = img->array.F;

  /*
    * This publisher thread is now the only writer.
    */
  img->md[0].write = 1;

  /*
    * Existing ImageStreamIO layout:
    *
    * index 0 = SC0 channel 1 = X
    * index 1 = SC0 channel 2 = Y
    * index 2 = SC1 channel 1 = Z
    * index 3 = unused
    */
  buffer[0] =
      (float)sc0Sample->channel[0] *
      publisher->sc0Scale[0];

  buffer[1] =
      (float)sc0Sample->channel[1] *
      publisher->sc0Scale[1];

  buffer[2] =
      (float)sc1Sample->channel[0] *
      publisher->sc1Scale[0];

  buffer[3] = 0.0f;

  /*
    * Ensure the sample data are visible before publishing
    * the metadata update.
    */
  atomic_thread_fence(memory_order_release);

  img->md[0].cnt0++;

  /*
    * Do not use cnt1 as an SC0/SC1 tag.
    */
  img->md[0].write = 0;

  atomic_thread_fence(memory_order_release);

  ImageStreamIO_sempost(img, -1);

  publisher->publishedFrames++;
}

static void *PUBLISHER_THREAD(void *argument) {
  PublisherContext *publisher =
      (PublisherContext *)argument;

  QueuedSample sc0Sample = {0};
  QueuedSample sc1Sample = {0};

  bool haveSc0 = false;
  bool haveSc1 = false;

  const struct timespec idleSleep = {
    .tv_sec = 0,
    .tv_nsec = 20000
  };

  while (true) {
    bool madeProgress = false;

    if (!haveSc0) {
      haveSc0 = SAMPLE_QUEUE_POP(
          publisher->sc0Queue,
          &sc0Sample
      );

      madeProgress |= haveSc0;
    }

    if (!haveSc1) {
      haveSc1 = SAMPLE_QUEUE_POP(
          publisher->sc1Queue,
          &sc1Sample
      );

      madeProgress |= haveSc1;
    }

      if (haveSc0 && haveSc1) {
        const double skewSeconds = sc0Sample.adcTime - sc1Sample.adcTime;

        const double absoluteSkewSeconds = fabs(skewSeconds);

        if (absoluteSkewSeconds <= publisher->matchToleranceSeconds) {
          PUBLISH_COMPLETE_FRAME(
              publisher,
              &sc0Sample,
              &sc1Sample
          );

          publisher->accumulatedAbsoluteSkewSeconds +=
              absoluteSkewSeconds;

          if (absoluteSkewSeconds > publisher->maximumAbsoluteSkewSeconds) {
            publisher->maximumAbsoluteSkewSeconds =
                absoluteSkewSeconds;
          }

          haveSc0 = false;
          haveSc1 = false;
          continue;
      }

          /*
            * Discard whichever sample is older.
            *
            * The next sample from that conditioner should
            * be closer to the other conditioner's sample.
            */
          if (skewSeconds < 0.0) {
              publisher->unmatchedSc0++;
              haveSc0 = false;
          } else {
              publisher->unmatchedSc1++;
              haveSc1 = false;
          }

          continue;
      }

      const bool stopping = atomic_load_explicit(
          &publisher->stopRequested,
          memory_order_acquire
      ) != 0;

      if (stopping) {
          const bool sc0Empty =
              SAMPLE_QUEUE_EMPTY(publisher->sc0Queue);

          const bool sc1Empty =
              SAMPLE_QUEUE_EMPTY(publisher->sc1Queue);

          /*
            * Drain unmatched samples after both PortAudio
            * streams have stopped.
            */
          if (haveSc0 && !haveSc1 && sc1Empty) {
              publisher->unmatchedSc0++;
              haveSc0 = false;
              continue;
          }

          if (haveSc1 && !haveSc0 && sc0Empty) {
              publisher->unmatchedSc1++;
              haveSc1 = false;
              continue;
          }

          if (
              !haveSc0 &&
              !haveSc1 &&
              sc0Empty &&
              sc1Empty
          ) {
              break;
          }
      }

      if (!madeProgress) {
          nanosleep(&idleSleep, NULL);
      }
  }

  return NULL;
}

static int CALLBACK(const void *inputBuffer, 
                    void *outputBuffer, 
                    unsigned long framesPerBuffer, 
                    const PaStreamCallbackTimeInfo* timeInfo, 
                    PaStreamCallbackFlags statusFlags, 
                    void *userData
                  ) {

  (void) outputBuffer;

  StreamContext *ctx = (StreamContext *)userData;

  SET_CALLBACK_AFFINITY(ctx);

  atomic_fetch_add_explicit(&ctx->callbackCount,1,memory_order_relaxed);
  
  if (statusFlags & paInputOverflow) {
    atomic_fetch_add_explicit(&ctx->inputOverflowCount,1,memory_order_relaxed);
  }
  

  if (statusFlags & ~paInputOverflow) {
    atomic_fetch_add_explicit(&ctx->otherStatusCount,1,memory_order_relaxed);
  }

  RECORD_TIMING(ctx, framesPerBuffer, timeInfo, statusFlags);

  // if (!ctx->printedRTProp) {
  //   ctx->printedRTProp = 1;

  //   PRINT_RT_PROPERTIES(ctx);
  // }

  if (inputBuffer == NULL) {
    atomic_fetch_add_explicit(&ctx->nullInputCount, 1, memory_order_relaxed);
    return paContinue;
  }

  if (timeInfo == NULL) {
    return paContinue;
  }

  ENQUEUE_DATA(
      (const int16_t *)inputBuffer,
      framesPerBuffer,
      timeInfo,
      ctx
  );
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
  

  if (err != paNoError) {
      fprintf(
          stderr,
          "Pa_StartStream error: %s\n",
          Pa_GetErrorText(err)
      );

      Pa_CloseStream(stream);
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
    if (Pa_IsStreamActive(stream0) == 1) {
      Pa_StopStream(stream0);
    }

    Pa_CloseStream(stream0);
  }

  if (stream1 != NULL) {
    if (Pa_IsStreamActive(stream1) == 1) {
      Pa_StopStream(stream1);
    }

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

static void RECORD_TIMING(StreamContext *ctx,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags) {
  if (ctx == NULL || timeInfo == NULL) {
      return;
  }

  const double expectedDelta = (double)framesPerBuffer / SAMPLE_RATE;
  const double adcTime       = timeInfo->inputBufferAdcTime;
  const double paCurrentTime = timeInfo->currentTime;
  const double linuxTime     = now_sec();
  const double paInputAge    = paCurrentTime - adcTime;
  const double clockOffset   = linuxTime - paCurrentTime;

  if (!ctx->timingInitialized) {
    ctx->previousAdcTime       = adcTime;
    ctx->previousPaCurrentTime = paCurrentTime;
    ctx->previousLinuxTime     = linuxTime;
    ctx->initialClockOffset    = clockOffset;
    ctx->minimumAdcDelta       = DBL_MAX;
    ctx->timingInitialized     = 1;
    return;
  }

  const double adcDelta       = adcTime - ctx->previousAdcTime;
  const double paCurrentDelta = paCurrentTime - ctx->previousPaCurrentTime;
  const double linuxDelta     = linuxTime - ctx->previousLinuxTime;

  const double clockOffsetChange = fabs(clockOffset - ctx->initialClockOffset);

  ctx->previousAdcTime       = adcTime;
  ctx->previousPaCurrentTime = paCurrentTime;
  ctx->previousLinuxTime     = linuxTime;

  if (adcDelta > 0.0) {
    if (adcDelta < ctx->minimumAdcDelta) {
      ctx->minimumAdcDelta = adcDelta;
    }

    if (adcDelta > ctx->maximumAdcDelta) {
      ctx->maximumAdcDelta = adcDelta;
    }
  }

  if (paCurrentDelta > ctx->maximumPaCurrentDelta) {
    ctx->maximumPaCurrentDelta = paCurrentDelta;
  }

  if (linuxDelta > ctx->maximumLinuxDelta) {
    ctx->maximumLinuxDelta = linuxDelta;
  }

  if (paInputAge > ctx->maximumPaInputAge) {
    ctx->maximumPaInputAge = paInputAge;
  }

  if (clockOffsetChange > ctx->maximumClockOffsetChange) {
    ctx->maximumClockOffsetChange = clockOffsetChange;
  }
  if (paCurrentDelta >= 0.002) { ctx->paDelayOver2ms++; }
  if (paCurrentDelta >= 0.003) { ctx->paDelayOver3ms++; }
  if (paCurrentDelta >= 0.004) { ctx->paDelayOver4ms++; }
  if (paCurrentDelta >= 0.005) { ctx->paDelayOver5ms++; }
  if (paCurrentDelta >= 0.010) {ctx->paDelayOver10ms++; }

  if (linuxDelta >= 0.002) { ctx->linuxDelayOver2ms++; }
  if (linuxDelta >= 0.003) { ctx->linuxDelayOver3ms++; }
  if (linuxDelta >= 0.004) { ctx->linuxDelayOver4ms++; }
  if (linuxDelta >= 0.005) { ctx->linuxDelayOver5ms++; }
  if (linuxDelta >= 0.010) { ctx->linuxDelayOver10ms++; }

  /*
   * Estimate the number of elapsed sample blocks according
   * to PortAudio's ADC timestamp.
   */
  uint64_t elapsedBlocks = 1;

  if (adcDelta > 0.0 && expectedDelta > 0.0) {
    const long long roundedBlocks = llround(adcDelta / expectedDelta);
    if (roundedBlocks > 1) {
      elapsedBlocks = (uint64_t)roundedBlocks;
    }
  }

  uint64_t missingFrames = 0;

  if (elapsedBlocks > 1) {
    missingFrames = (elapsedBlocks - 1) * (uint64_t)framesPerBuffer;
    ctx->adcDiscontinuityCount++;
    ctx->estimatedMissingFrames += missingFrames;
  }

  const int adcJump = elapsedBlocks > 1;

  // Arbitrary number here tbh
  const int severePaDelay = paCurrentDelta >= SEVERE_PA_DELAY_S;
  const int severeLinuxDelay = linuxDelta >= SEVERE_LINUX_DELAY_S;

  const int portAudioError = statusFlags != 0;


  // Record timing event if any of the conditions are met
  uint32_t eventReasons = 0;

  if (adcJump) {
    eventReasons |= TIMING_EVENT_ADC_JUMP;
  }

  if (severePaDelay) {
    eventReasons |= TIMING_EVENT_PA_DELAY;
  }

  if (severeLinuxDelay) {
    eventReasons |= TIMING_EVENT_LINUX_DELAY;
  }

  if (portAudioError) {
    eventReasons |= TIMING_EVENT_PA_STATUS;
  }


  if (eventReasons != 0) {
    if (ctx->timingEventCount < ctx->timingEventCapacity) {
      TimingEvent *event = &ctx->timingEvents[ctx->timingEventCount++];

      event->callbackIndex = atomic_load_explicit(&ctx->callbackCount, memory_order_relaxed);

      event->adcTime = adcTime;
      event->adcDelta = adcDelta;

      event->paCurrentTime = paCurrentTime;
      event->paCurrentDelta = paCurrentDelta;

      event->linuxTime = linuxTime;
      event->linuxDelta = linuxDelta;

      event->paInputAge = paInputAge;
      event->clockOffsetChange = clockOffsetChange;

      event->estimatedMissingFrames = missingFrames;
      event->statusFlags = statusFlags;
      event->eventReasons = eventReasons;
    } else {
      ctx->discardedTimingEvents++;
    }
  }
}

static int WRITE_TIMING_FILE(const char *filename, const StreamContext *ctx)
{
  FILE *file = fopen(filename, "w");

  if (file == NULL) {
    perror(filename);
    return -1;
  }


  fprintf(
      file,
      "callback_index,"
      "adc_time_s,"
      "adc_delta_s,"
      "pa_current_time_s,"
      "pa_current_delta_s,"
      "linux_time_s,"
      "linux_delta_s,"
      "pa_input_age_s,"
      "clock_offset_change_s,"
      "estimated_missing_frames,"
      "status_flags,"
      "event_reasons\n");

  for (size_t i = 0; i < ctx->timingEventCount; ++i) {
    const TimingEvent *event = &ctx->timingEvents[i];

    fprintf(
        file,
        "%" PRIu64 ","
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%.12f,"
        "%" PRIu64 ","
        "0x%lx,"
        "%" PRIx32 "\n",
        event->callbackIndex,
        event->adcTime,
        event->adcDelta,
        event->paCurrentTime,
        event->paCurrentDelta,
        event->linuxTime,
        event->linuxDelta,
        event->paInputAge,
        event->clockOffsetChange,
        event->estimatedMissingFrames,
        (unsigned long)event->statusFlags,
        event->eventReasons);
        
  }

  if (fclose(file) != 0) {
    perror("fclose");
    return -1;
  }

  return 0;
}

static int WRITE_TIMING_SUMMARY_FILE(const char *filename,
                                     const StreamContext *ctx,
                                     double durationSeconds)
{
    FILE *file = fopen(filename, "w");

    if (file == NULL) {
      perror(filename);
      return -1;
    }

    const uint64_t callbacks = atomic_load_explicit(&ctx->callbackCount, memory_order_relaxed);

    const uint64_t overflows = atomic_load_explicit(&ctx->inputOverflowCount, memory_order_relaxed);

    const uint64_t other = atomic_load_explicit(&ctx->otherStatusCount, memory_order_relaxed);

    const uint64_t nullInputs = atomic_load_explicit(&ctx->nullInputCount, memory_order_relaxed);

    fprintf(file,
            "stream,"
            "duration_s,"
            "sample_rate_hz,"
            "callbacks,"
            "input_overflows,"
            "other_status_events,"
            "null_input_buffers,"
            "adc_discontinuities,"
            "estimated_skipped_intervals,"
            "pa_over_2ms,"
            "pa_over_3ms,"
            "pa_over_4ms,"
            "pa_over_5ms,"
            "pa_over_10ms,"
            "linux_over_2ms,"
            "linux_over_3ms,"
            "linux_over_4ms,"
            "linux_over_5ms,"
            "linux_over_10ms,"
            "adc_delta_min_us,"
            "adc_delta_max_us,"
            "pa_delta_max_us,"
            "linux_delta_max_us,"
            "input_age_max_us,"
            "clock_offset_change_max_us\n");

    fprintf(file,
            "%s,"
            "%.6f,"
            "%.0f,"
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%" PRIu64 ","
            "%.3f,"
            "%.3f,"
            "%.3f,"
            "%.3f,"
            "%.3f,"
            "%.3f\n",
            ctx->name,
            durationSeconds,
            SAMPLE_RATE,
            callbacks,
            overflows,
            other,
            nullInputs,
            ctx->adcDiscontinuityCount,
            ctx->estimatedMissingFrames,
            ctx->paDelayOver2ms,
            ctx->paDelayOver3ms,
            ctx->paDelayOver4ms,
            ctx->paDelayOver5ms,
            ctx->paDelayOver10ms,
            ctx->linuxDelayOver2ms,
            ctx->linuxDelayOver3ms,
            ctx->linuxDelayOver4ms,
            ctx->linuxDelayOver5ms,
            ctx->linuxDelayOver10ms,
            ctx->minimumAdcDelta * 1.0e6,
            ctx->maximumAdcDelta * 1.0e6,
            ctx->maximumPaCurrentDelta * 1.0e6,
            ctx->maximumLinuxDelta * 1.0e6,
            ctx->maximumPaInputAge * 1.0e6,
            ctx->maximumClockOffsetChange * 1.0e6);

    if (fclose(file) != 0) {
      perror("fclose");
      return -1;
    }

    return 0;
}

static void PRINT_TIMING_SUMMARY(const StreamContext *ctx, double durationSeconds) {
  const double expectedUs = 1.0e6 * (double)FRAMES_PER_BUFFER / SAMPLE_RATE;

  const uint64_t callbacks  = atomic_load_explicit(&ctx->callbackCount, memory_order_relaxed);
  const uint64_t overflows  = atomic_load_explicit(&ctx->inputOverflowCount,memory_order_relaxed);
  const uint64_t other      = atomic_load_explicit(&ctx->otherStatusCount,memory_order_relaxed);
  const uint64_t nullInputs = atomic_load_explicit(&ctx->nullInputCount, memory_order_relaxed);
  const double discontinuityRate = durationSeconds > 0.0 
        ? (double)ctx->adcDiscontinuityCount / durationSeconds
        : 0.0;

  const double missingFrameRate = durationSeconds > 0.0
        ? (double)ctx->estimatedMissingFrames / durationSeconds
        : 0.0;

  printf("\nTiming summary for %s\n", ctx->name);

  printf("Expected sample-block period:           %.3f us\n", expectedUs);
  printf("Duration:                               %.3f s\n", durationSeconds);
  printf("Sample rate:                            %.0f Hz\n", SAMPLE_RATE);
  printf("Callbacks:                              %" PRIu64 "\n\n", callbacks);

  printf("Input overflows:                        %" PRIu64 "\n", overflows);
  printf("Other status events::                   %" PRIu64 "\n", other);
  printf("Null input buffers:                     %" PRIu64 "\n", nullInputs);

  printf("ADC discontinuities:                    %" PRIu64 "\n", ctx->adcDiscontinuityCount);
  printf("Estimated skipped sample interval:      %" PRIu64 "\n", ctx->estimatedMissingFrames);
  printf("Discontinuity rate:                     %.3f / s\n", discontinuityRate);
  printf("Estimated skipped sample interval rate: %.3f / s\n\n", missingFrameRate);
  printf("\nPortAudio callback delay distribution\n");
  printf("Intervals >= 2 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->paDelayOver2ms, rate_per_second(ctx->paDelayOver2ms, durationSeconds));
  printf("Intervals >= 3 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->paDelayOver3ms, rate_per_second(ctx->paDelayOver3ms, durationSeconds));
  printf("Intervals >= 4 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->paDelayOver4ms, rate_per_second(ctx->paDelayOver4ms, durationSeconds));
  printf("Intervals >= 5 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->paDelayOver5ms, rate_per_second(ctx->paDelayOver5ms, durationSeconds));
  printf("Intervals >= 10 ms:                     %" PRIu64 "  (%.3f / s)\n",
        ctx->paDelayOver10ms, rate_per_second(ctx->paDelayOver10ms, durationSeconds));

  printf("\nLinux callback delay distribution\n");
  printf("Intervals >= 2 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->linuxDelayOver2ms, rate_per_second(ctx->linuxDelayOver2ms, durationSeconds));
  printf("Intervals >= 3 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->linuxDelayOver3ms, rate_per_second(ctx->linuxDelayOver3ms, durationSeconds));
  printf("Intervals >= 4 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->linuxDelayOver4ms, rate_per_second(ctx->linuxDelayOver4ms, durationSeconds));
  printf("Intervals >= 5 ms:                      %" PRIu64 "  (%.3f / s)\n",
        ctx->linuxDelayOver5ms, rate_per_second(ctx->linuxDelayOver5ms, durationSeconds));
  printf("Intervals >= 10 ms:                     %" PRIu64 "  (%.3f / s)\n",
        ctx->linuxDelayOver10ms, rate_per_second(ctx->linuxDelayOver10ms, durationSeconds));

  if (ctx->timingInitialized && ctx->minimumAdcDelta != DBL_MAX) {
    printf("\nADC delta min:                          %.3f us\n", ctx->minimumAdcDelta * 1.0e6);
  } else {
    printf("ADC delta min:              unavailable\n");
  }

  printf("ADC delta max:                          %.3f us\n", ctx->maximumAdcDelta * 1.0e6);
  printf("PortAudio callback max:                 %.3f us\n", ctx->maximumPaCurrentDelta * 1.0e6);
  printf("Linux callback max:                     %.3f us\n", ctx->maximumLinuxDelta * 1.0e6);
  printf("PortAudio input age max:                %.3f us\n", ctx->maximumPaInputAge * 1.0e6);
  printf("Clock offset change max:                %.3f us\n", ctx->maximumClockOffsetChange * 1.0e6);

  printf("Stored timing events:                   %zu\n", ctx->timingEventCount);
  printf("Discarded timing events:                %" PRIu64 "\n", ctx->discardedTimingEvents);
}

// Commented out for now, as it was causing issues with PortAudio callbacks and CPU affinity
// When analyzing with analyze_timing.py, the jump rate was about double what it was without the setting 
// the CPU affinity. I think has to do with both callbacks posting semaphore to the same shm img, 
// and the CPU affinity causing one callback to be delayed more than the other. Causing drops and inaccurate readings.
// Not fully sure if the Z axis (sc1_ch1) data is accurate. Would probably be fixed if port audio callbacks writes to a buffer 
// and seperate thread synchronizes the buffer to shm img. But for now, just leaving it commented out due to limited time. 
// Just want to have reliable enough data to present and show at the symposium. 

static void SET_CALLBACK_AFFINITY(StreamContext *ctx) {
    if (atomic_load_explicit(&ctx->affinityState, memory_order_relaxed) != 0) {
        return;
    }

    pid_t tid = syscall(SYS_gettid);

    if (ctx->targetCpu < 0 || ctx->targetCpu >= CPU_SETSIZE) {
        atomic_store_explicit(&ctx->callbackTid, (int)tid, memory_order_relaxed);
        atomic_store_explicit(&ctx->affinityError, EINVAL, memory_order_relaxed);
        atomic_store_explicit(&ctx->affinityState, -1, memory_order_release);
        return;
    }

    cpu_set_t cpuMask;
    CPU_ZERO(&cpuMask);
    CPU_SET(ctx->targetCpu, &cpuMask);

    if (sched_setaffinity(0, sizeof(cpuMask), &cpuMask) != 0) {
        atomic_store_explicit(&ctx->callbackTid, (int)tid, memory_order_relaxed);
        atomic_store_explicit(&ctx->affinityError, errno, memory_order_relaxed);
        atomic_store_explicit(&ctx->affinityState, -1, memory_order_release);
        return;
    }

    atomic_store_explicit(&ctx->callbackTid, (int)tid, memory_order_relaxed);
    atomic_store_explicit(&ctx->callbackCpu, sched_getcpu(), memory_order_relaxed);
    atomic_store_explicit(&ctx->affinityState, 1, memory_order_release);
}

// static int SET_CURRENT_THREAD_CPU(int cpu) {
//     if (cpu < 0 || cpu >= CPU_SETSIZE) {
//         errno = EINVAL;
//         return -1;
//     }

//     cpu_set_t cpuMask;
//     CPU_ZERO(&cpuMask);
//     CPU_SET(cpu, &cpuMask);

//     return sched_setaffinity(0, sizeof(cpuMask), &cpuMask);
// }

// static void PRINT_CALLBACK_AFFINITY(const StreamContext *ctx) {
//     int state = atomic_load_explicit(&ctx->affinityState, memory_order_acquire);
//     int tid = atomic_load_explicit(&ctx->callbackTid, memory_order_relaxed);

//     if (state == 1) {
//         int cpu = atomic_load_explicit(&ctx->callbackCpu, memory_order_relaxed);

//         printf("%s callback TID %d pinned to CPU %d\n", ctx->name, tid, cpu);
//     } else if (state == -1) {
//         int errorNumber = atomic_load_explicit(&ctx->affinityError, memory_order_relaxed);

//         printf("%s callback TID %d affinity failed: %s\n", ctx->name, tid, strerror(errorNumber));
//     } else {
//         printf("%s callback affinity has not been configured yet.\n", ctx->name);
//     }
// }