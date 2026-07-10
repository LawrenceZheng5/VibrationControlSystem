#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include "common.h"
#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"

static volatile sig_atomic_t keepRunning = 1;

static void HANDLE_SIGNAL(int signo) {
  (void) signo;
  keepRunning = 0;
}

int main(int argc, char *argv[])
{
    const char *streamName = "sig00";

    if (argc >= 2) {
        streamName = argv[1];
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = HANDLE_SIGNAL;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        // Not fatal for a monitor, so continue.
    }

    IMAGE img;
    memset(&img, 0, sizeof(IMAGE));

    int ret = ImageStreamIO_openIm(&img, streamName);
    if (ret != 0) {
        fprintf(stderr, "Failed to open stream: %s\n", streamName);
        return 1;
    }

    printf("Monitoring stream: %s\n", streamName);
    printf("Press Ctrl+C to stop.\n\n");

    uint64_t prevCnt0 = img.md[0].cnt0;
    uint64_t currentCnt0 = prevCnt0;

    uint64_t totalUpdatesSeen = 0;
    uint64_t missedUpdates = 0;
    uint64_t jumpEvents = 0;
    uint64_t maxJump = 0;

    uint64_t sc0Updates = 0;
    uint64_t sc1Updates = 0;

    double tStart = now_sec();
    // double tLastPrint = tStart;

    while (keepRunning) {
        /*
         * Wait for the producer to post the stream semaphore.
         */
        
        ImageStreamIO_semwait(&img, 0);

        // ImageStreamIO_semtrywait(&img, 0);

        currentCnt0 = img.md[0].cnt0;
        uint64_t sourceTag = img.md[0].cnt1;

        if (currentCnt0 == prevCnt0) {
            /*
             * This can happen if the semaphore was already posted,
             * or if metadata was read during a race.
             * Not necessarily a dropped frame.
             */
            continue;
        }

        uint64_t delta = currentCnt0 - prevCnt0;

        if (delta > 1) {
            uint64_t missed = delta - 1;
            missedUpdates += missed;
            jumpEvents++;

            if (delta > maxJump) {
                maxJump = delta;
            }

            // printf("Jump detected: prev=%lu current=%lu delta=%lu missed=%lu sourceTag=%lu\n",
            //        prevCnt0,
            //        currentCnt0,
            //        delta,
            //        missed,
            //        sourceTag);
        }

        totalUpdatesSeen++;

        if (sourceTag == 0) {
            sc0Updates++;
        } else if (sourceTag == 1) {
            sc1Updates++;
        }

        prevCnt0 = currentCnt0;

        // double tNow = now_sec();

        // if (tNow - tLastPrint >= 5.0) {
        //     // double elapsed = tNow - tStart;
        //     // double updateRate = totalUpdatesSeen / elapsed;

        //     // printf("[%.1f s] seen=%lu missed=%lu jumps=%lu maxDelta=%lu rate=%.2f Hz sc0=%lu sc1=%lu lastCnt0=%lu lastSrc=%lu\n",
        //     //        elapsed,
        //     //        totalUpdatesSeen,
        //     //        missedUpdates,
        //     //        jumpEvents,
        //     //        maxJump,
        //     //        updateRate,
        //     //        sc0Updates,
        //     //        sc1Updates,
        //     //        currentCnt0,
        //     //        sourceTag);

        //     tLastPrint = tNow;
        // }
    }

    double tEnd = now_sec();
    double elapsed = tEnd - tStart;

    printf("\nStopping monitor...\n");
    printf("Stream: %s\n", streamName);
    printf("Elapsed: %.3f s\n", elapsed);
    printf("Updates seen: %lu\n", totalUpdatesSeen);
    printf("Missed updates: %lu\n", missedUpdates);
    printf("Jump events: %lu\n", jumpEvents);
    printf("Max cnt0 delta: %lu\n", maxJump);
    printf("Average observed update rate: %.3f Hz\n", totalUpdatesSeen / elapsed);
    printf("SC0-tagged updates: %lu\n", sc0Updates);
    printf("SC1-tagged updates: %lu\n", sc1Updates);
    printf("Final cnt0: %lu\n", currentCnt0);

    ImageStreamIO_closeIm(&img);
    munlockall();

    return 0;
}