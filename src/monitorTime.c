#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

#include "ImageStreamIO/ImageStreamIO.h"
#include "ImageStreamIO/ImageStruct.h"

static volatile sig_atomic_t keepRunning = 1;

static void handle_signal(int signo)
{
    (void) signo;
    keepRunning = 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

int main(int argc, char *argv[])
{
    const char *streamName = "sig00";

    /*
     * Default expected update rate:
     * Two 8 kHz signal conditioners both posting to the same stream
     * gives roughly 16 kHz total updates.
     */
    double expectedRateHz = 16000.0;
    double thresholdFrac = 0.10;  // 10%

    if (argc >= 2) {
        streamName = argv[1];
    }

    if (argc >= 3) {
        expectedRateHz = atof(argv[2]);
        if (expectedRateHz <= 0.0) {
            fprintf(stderr, "Invalid expected rate: %s\n", argv[2]);
            return 1;
        }
    }

    if (argc >= 4) {
        thresholdFrac = atof(argv[3]);
        if (thresholdFrac < 0.0) {
            fprintf(stderr, "Invalid threshold fraction: %s\n", argv[3]);
            return 1;
        }
    }

    const double expectedPeriodSec = 1.0 / expectedRateHz;
    const double minPeriodSec = expectedPeriodSec * (1.0 - thresholdFrac);
    const double maxPeriodSec = expectedPeriodSec * (1.0 + thresholdFrac);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        // Not fatal for a monitor.
    }

    IMAGE img;
    memset(&img, 0, sizeof(IMAGE));

    int ret = ImageStreamIO_openIm(&img, streamName);
    if (ret != 0) {
        fprintf(stderr, "Failed to open stream: %s\n", streamName);
        return 1;
    }

    printf("Monitoring stream: %s\n", streamName);
    printf("Expected update rate: %.3f Hz\n", expectedRateHz);
    printf("Expected period: %.3f us\n", expectedPeriodSec * 1.0e6);
    printf("Allowed range: %.3f us to %.3f us\n",
           minPeriodSec * 1.0e6,
           maxPeriodSec * 1.0e6);
    printf("Threshold: +/- %.1f %%\n", thresholdFrac * 100.0);
    printf("Press Ctrl+C to stop.\n\n");

    uint64_t prevCnt0 = img.md[0].cnt0;
    uint64_t currentCnt0 = prevCnt0;

    uint64_t totalUpdatesSeen = 0;
    uint64_t missedUpdates = 0;
    uint64_t jumpEvents = 0;
    uint64_t maxCntDelta = 0;

    uint64_t timingGood = 0;
    uint64_t timingBad = 0;
    uint64_t timingEarly = 0;
    uint64_t timingLate = 0;

    uint64_t sc0Updates = 0;
    uint64_t sc1Updates = 0;

    double minObservedDt = 1.0e99;
    double maxObservedDt = 0.0;
    double sumObservedDt = 0.0;

    double tStart = now_sec();
    double tLastPrint = tStart;
    double tPrevUpdate = 0.0;

    while (keepRunning) {
        /*
         * Wait until producer posts the stream semaphore.
         * This avoids spinning the CPU.
         */
        ImageStreamIO_semwait(&img, 0);

        double tNow = now_sec();

        currentCnt0 = img.md[0].cnt0;
        uint64_t sourceTag = img.md[0].cnt1;

        if (currentCnt0 == prevCnt0) {
            /*
             * Spurious wakeup or metadata race.
             * Do not count it as a real stream update.
             */
            continue;
        }

        uint64_t cntDelta = currentCnt0 - prevCnt0;

        /*
         * If cntDelta > 1, the reader observed a jump in cnt0.
         * That means the monitor did not see every update individually.
         */
        if (cntDelta > 1) {
            uint64_t missed = cntDelta - 1;
            missedUpdates += missed;
            jumpEvents++;

            if (cntDelta > maxCntDelta) {
                maxCntDelta = cntDelta;
            }

            printf("CNT JUMP: prev=%lu current=%lu delta=%lu missed=%lu sourceTag=%lu\n",
                   prevCnt0,
                   currentCnt0,
                   cntDelta,
                   missed,
                   sourceTag);
        }

        /*
         * Timing check.
         *
         * If cntDelta == 1, expected time is one update period.
         * If cntDelta > 1, expected time is cntDelta periods.
         */
        if (tPrevUpdate > 0.0) {
            double observedDt = tNow - tPrevUpdate;
            double expectedDt = expectedPeriodSec * (double)cntDelta;
            double minDt = expectedDt * (1.0 - thresholdFrac);
            double maxDt = expectedDt * (1.0 + thresholdFrac);

            sumObservedDt += observedDt;

            if (observedDt < minObservedDt) {
                minObservedDt = observedDt;
            }

            if (observedDt > maxObservedDt) {
                maxObservedDt = observedDt;
            }

            if (observedDt < minDt) {
                timingBad++;
                timingEarly++;

                printf("TIMING EARLY: dt=%.3f us expected=%.3f us range=[%.3f, %.3f] us cntDelta=%lu sourceTag=%lu\n",
                       observedDt * 1.0e6,
                       expectedDt * 1.0e6,
                       minDt * 1.0e6,
                       maxDt * 1.0e6,
                       cntDelta,
                       sourceTag);
            } else if (observedDt > maxDt) {
                timingBad++;
                timingLate++;

                printf("TIMING LATE:  dt=%.3f us expected=%.3f us range=[%.3f, %.3f] us cntDelta=%lu sourceTag=%lu\n",
                       observedDt * 1.0e6,
                       expectedDt * 1.0e6,
                       minDt * 1.0e6,
                       maxDt * 1.0e6,
                       cntDelta,
                       sourceTag);
            } else {
                timingGood++;
            }
        }

        totalUpdatesSeen++;

        if (sourceTag == 0) {
            sc0Updates++;
        } else if (sourceTag == 1) {
            sc1Updates++;
        }

        prevCnt0 = currentCnt0;
        tPrevUpdate = tNow;

        if (tNow - tLastPrint >= 5.0) {
            double elapsed = tNow - tStart;
            double observedRate = totalUpdatesSeen / elapsed;
            double avgDt = 0.0;

            if ((timingGood + timingBad) > 0) {
                avgDt = sumObservedDt / (double)(timingGood + timingBad);
            }

            printf("[%.1f s] seen=%lu missed=%lu jumps=%lu rate=%.2f Hz "
                   "timingGood=%lu timingBad=%lu early=%lu late=%lu "
                   "avgDt=%.3f us minDt=%.3f us maxDt=%.3f us "
                   "sc0=%lu sc1=%lu lastCnt0=%lu lastSrc=%lu\n",
                   elapsed,
                   totalUpdatesSeen,
                   missedUpdates,
                   jumpEvents,
                   observedRate,
                   timingGood,
                   timingBad,
                   timingEarly,
                   timingLate,
                   avgDt * 1.0e6,
                   minObservedDt * 1.0e6,
                   maxObservedDt * 1.0e6,
                   sc0Updates,
                   sc1Updates,
                   currentCnt0,
                   sourceTag);

            tLastPrint = tNow;
        }
    }

    double tEnd = now_sec();
    double elapsed = tEnd - tStart;
    double observedRate = totalUpdatesSeen / elapsed;

    double avgDt = 0.0;
    if ((timingGood + timingBad) > 0) {
        avgDt = sumObservedDt / (double)(timingGood + timingBad);
    }

    printf("\nStopping monitor...\n");
    printf("Stream: %s\n", streamName);
    printf("Elapsed: %.3f s\n", elapsed);
    printf("Updates seen: %lu\n", totalUpdatesSeen);
    printf("Average observed update rate: %.3f Hz\n", observedRate);

    printf("\nCounter check:\n");
    printf("Missed updates from cnt0 jumps: %lu\n", missedUpdates);
    printf("Jump events: %lu\n", jumpEvents);
    printf("Max cnt0 delta: %lu\n", maxCntDelta);

    printf("\nTiming check:\n");
    printf("Expected period: %.3f us\n", expectedPeriodSec * 1.0e6);
    printf("Allowed single-update range: %.3f us to %.3f us\n",
           minPeriodSec * 1.0e6,
           maxPeriodSec * 1.0e6);
    printf("Timing good: %lu\n", timingGood);
    printf("Timing bad: %lu\n", timingBad);
    printf("Timing early: %lu\n", timingEarly);
    printf("Timing late: %lu\n", timingLate);
    printf("Average observed dt: %.3f us\n", avgDt * 1.0e6);
    printf("Min observed dt: %.3f us\n", minObservedDt * 1.0e6);
    printf("Max observed dt: %.3f us\n", maxObservedDt * 1.0e6);

    printf("\nSource tags:\n");
    printf("SC0-tagged updates: %lu\n", sc0Updates);
    printf("SC1-tagged updates: %lu\n", sc1Updates);
    printf("Final cnt0: %lu\n", currentCnt0);

    ImageStreamIO_closeIm(&img);
    munlockall();

    return 0;
}