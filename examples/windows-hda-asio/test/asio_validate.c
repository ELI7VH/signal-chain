/*
 * asio_validate.c -- Buffer capture + validation test for HDA Direct ASIO
 *
 * Loads the ASIO driver, streams for a few seconds while recording
 * every input and output buffer, then compares them sample-by-sample
 * to verify the loopback path is bit-perfect.
 *
 * Also dumps captured audio to WAV files for inspection.
 *
 * Build (MinGW/w64devkit):
 *   gcc -O2 -Wall -o asio_validate.exe ../test/asio_validate.c -lole32 -loleaut32
 */

#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "../include/asio.h"

/* ---- Configuration ---- */

#define RECORD_SECONDS  3       /* How long to capture */
#define SAMPLE_RATE     48000
#define MAX_FRAMES      (SAMPLE_RATE * RECORD_SECONDS)

/* ---- Globals ---- */

static volatile int g_running = 1;
static IASIO *g_asio = NULL;
static long g_bufferSize = 0;
static long g_numInputCh = 0;
static long g_numOutputCh = 0;

static ASIOBufferInfo g_bufferInfos[4];

/* Recording buffers -- per-channel mono int16 */
static short *g_recInput[2]  = { NULL, NULL };   /* [channel] */
static short *g_recOutput[2] = { NULL, NULL };
static long   g_recPos = 0;                       /* current write position (frames) */
static long   g_recMax = 0;                        /* MAX_FRAMES */

static volatile long long g_callbackCount = 0;

/* ---- Signal handler ---- */

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- ASIO Callbacks ---- */

static void ASIO_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
    (void)directProcess;
    long i;
    long framesLeft = g_recMax - g_recPos;
    long framesToCopy = g_bufferSize;
    if (framesToCopy > framesLeft)
        framesToCopy = framesLeft;

    /* Record input buffers (capture what the mic picked up) */
    for (i = 0; i < g_numInputCh && i < 2; i++) {
        short *src = (short *)g_bufferInfos[i].buffers[doubleBufferIndex];
        if (src && framesToCopy > 0) {
            memcpy(&g_recInput[i][g_recPos], src, framesToCopy * sizeof(short));
        }
    }

    /* Loopback: copy input -> output (per-channel, non-interleaved) */
    for (i = 0; i < g_numInputCh && i < g_numOutputCh; i++) {
        long inIdx = i;
        long outIdx = g_numInputCh + i;

        if (inIdx < 4 && outIdx < 4) {
            void *src = g_bufferInfos[inIdx].buffers[doubleBufferIndex];
            void *dst = g_bufferInfos[outIdx].buffers[doubleBufferIndex];

            if (src && dst) {
                memcpy(dst, src, g_bufferSize * sizeof(short));
            }
        }
    }

    /* Record output buffers (what we just wrote -- should match input) */
    for (i = 0; i < g_numOutputCh && i < 2; i++) {
        long outIdx = g_numInputCh + i;
        if (outIdx < 4) {
            short *src = (short *)g_bufferInfos[outIdx].buffers[doubleBufferIndex];
            if (src && framesToCopy > 0) {
                memcpy(&g_recOutput[i][g_recPos], src, framesToCopy * sizeof(short));
            }
        }
    }

    g_recPos += framesToCopy;
    g_callbackCount++;

    /* Auto-stop after recording enough */
    if (g_recPos >= g_recMax) {
        g_running = 0;
    }
}

static void ASIO_sampleRateDidChange(ASIOSampleRate sRate)
{
    printf("Sample rate changed to: %.0f\n", sRate);
}

static long ASIO_asioMessage(long selector, long value, void *message, double *opt)
{
    (void)value; (void)message; (void)opt;
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
        case kAsioSupportsTimeInfo:
            return 1;
        }
        return 0;
    case kAsioEngineVersion:
        return 2;
    default:
        return 0;
    }
}

static ASIOCallbacks g_callbacks = {
    ASIO_bufferSwitch,
    ASIO_sampleRateDidChange,
    ASIO_asioMessage,
    NULL
};

/* ---- WAV writer (16-bit mono) ---- */

static int write_wav_mono(const char *path, const short *data, long numSamples, int sampleRate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("Cannot open %s for writing\n", path);
        return -1;
    }

    int numChannels = 1;
    int bitsPerSample = 16;
    int blockAlign = numChannels * bitsPerSample / 8;
    int byteRate = sampleRate * blockAlign;
    int dataSize = numSamples * blockAlign;
    int riffSize = 36 + dataSize;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    int fmtSize = 16;
    short fmtTag = 1;  /* PCM */
    short nCh = (short)numChannels;
    short bps = (short)bitsPerSample;
    short ba = (short)blockAlign;
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&fmtTag, 2, 1, f);
    fwrite(&nCh, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    fwrite(data, sizeof(short), numSamples, f);

    fclose(f);
    return 0;
}

/* ---- Validation ---- */

static void validate_buffers(long totalFrames)
{
    int ch;
    long totalMismatches = 0;
    long totalSamples = 0;

    printf("\n=== Buffer Validation ===\n\n");

    for (ch = 0; ch < 2 && ch < g_numInputCh && ch < g_numOutputCh; ch++) {
        long mismatches = 0;
        long firstMismatch = -1;
        long i;

        for (i = 0; i < totalFrames; i++) {
            if (g_recInput[ch][i] != g_recOutput[ch][i]) {
                mismatches++;
                if (firstMismatch < 0) firstMismatch = i;
            }
        }

        totalMismatches += mismatches;
        totalSamples += totalFrames;

        if (mismatches == 0) {
            printf("  Channel %d: PASS -- %ld samples, bit-perfect match\n",
                   ch, totalFrames);
        } else {
            printf("  Channel %d: FAIL -- %ld / %ld mismatches (first at sample %ld)\n",
                   ch, mismatches, totalFrames, firstMismatch);

            /* Show first few mismatches */
            long shown = 0;
            for (i = 0; i < totalFrames && shown < 5; i++) {
                if (g_recInput[ch][i] != g_recOutput[ch][i]) {
                    printf("    [%ld] input=%d  output=%d  diff=%d\n",
                           i, g_recInput[ch][i], g_recOutput[ch][i],
                           g_recInput[ch][i] - g_recOutput[ch][i]);
                    shown++;
                }
            }
        }
    }

    printf("\n  TOTAL: %ld / %ld samples checked\n", totalSamples, totalSamples);
    if (totalMismatches == 0) {
        printf("  RESULT: ALL CHANNELS BIT-PERFECT\n");
    } else {
        printf("  RESULT: %ld MISMATCHES FOUND\n", totalMismatches);
    }

    /* Check for duplicate-pair pattern (old interleaving bug) */
    printf("\n=== Interleaving Check ===\n\n");
    for (ch = 0; ch < 2 && ch < g_numInputCh; ch++) {
        long dupPairs = 0;
        long i;
        for (i = 0; i < totalFrames - 1; i += 2) {
            if (g_recInput[ch][i] == g_recInput[ch][i + 1]) {
                dupPairs++;
            }
        }
        long totalPairs = totalFrames / 2;
        double pct = (totalPairs > 0) ? (100.0 * dupPairs / totalPairs) : 0;
        if (pct > 80.0) {
            printf("  Channel %d: WARNING -- %.1f%% duplicate pairs detected!\n", ch, pct);
            printf("             This suggests interleaved data in a non-interleaved buffer.\n");
        } else {
            printf("  Channel %d: OK -- %.1f%% duplicate pairs (expected for real audio)\n",
                   ch, pct);
        }
    }
}

/* ---- Main ---- */

int main(void)
{
    HRESULT hr;
    ASIOError err;
    long minSize, maxSize, preferredSize, granularity;
    long i;

    printf("=== signal-chain: ASIO Buffer Validation Test ===\n\n");
    printf("Will record %d seconds of loopback and validate buffers.\n\n", RECORD_SECONDS);

    signal(SIGINT, sigint_handler);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    hr = CoCreateInstance(&CLSID_HdaDirectAsio, NULL, CLSCTX_INPROC_SERVER,
                          &CLSID_HdaDirectAsio, (void **)&g_asio);
    if (FAILED(hr)) {
        printf("Cannot create ASIO instance: 0x%08lX\n", hr);
        CoUninitialize();
        return 1;
    }

    printf("Initializing...\n");
    if (!g_asio->lpVtbl->init(g_asio, NULL)) {
        char errMsg[124];
        g_asio->lpVtbl->getErrorMessage(g_asio, errMsg);
        printf("ASIO init failed: %s\n", errMsg);
        g_asio->lpVtbl->Release(g_asio);
        CoUninitialize();
        return 1;
    }

    g_asio->lpVtbl->getChannels(g_asio, &g_numInputCh, &g_numOutputCh);
    printf("Channels: %ld in, %ld out\n", g_numInputCh, g_numOutputCh);

    g_asio->lpVtbl->getBufferSize(g_asio, &minSize, &maxSize, &preferredSize, &granularity);
    g_bufferSize = preferredSize;
    printf("Buffer: %ld frames\n", g_bufferSize);

    /* Allocate recording buffers */
    g_recMax = MAX_FRAMES;
    for (i = 0; i < 2 && i < g_numInputCh; i++) {
        g_recInput[i] = (short *)calloc(g_recMax, sizeof(short));
    }
    for (i = 0; i < 2 && i < g_numOutputCh; i++) {
        g_recOutput[i] = (short *)calloc(g_recMax, sizeof(short));
    }
    g_recPos = 0;

    /* Create ASIO buffers */
    long numChannels = g_numInputCh + g_numOutputCh;
    if (numChannels > 4) numChannels = 4;

    memset(g_bufferInfos, 0, sizeof(g_bufferInfos));
    long idx = 0;
    for (i = 0; i < g_numInputCh && idx < 4; i++, idx++) {
        g_bufferInfos[idx].isInput = ASIOTrue;
        g_bufferInfos[idx].channelIndex = i;
    }
    for (i = 0; i < g_numOutputCh && idx < 4; i++, idx++) {
        g_bufferInfos[idx].isInput = ASIOFalse;
        g_bufferInfos[idx].channelIndex = i;
    }

    err = g_asio->lpVtbl->createBuffers(g_asio, g_bufferInfos, numChannels,
                                         g_bufferSize, &g_callbacks);
    if (err != ASE_OK) {
        printf("createBuffers failed (error=%ld)\n", err);
        goto cleanup;
    }

    /* Start streaming */
    printf("Starting %d-second capture...\n", RECORD_SECONDS);
    err = g_asio->lpVtbl->start(g_asio);
    if (err != ASE_OK) {
        char errMsg[124];
        g_asio->lpVtbl->getErrorMessage(g_asio, errMsg);
        printf("ASIO start failed: %s (error=%ld)\n", errMsg, err);
        goto cleanup;
    }

    /* Wait for recording to complete */
    while (g_running) {
        Sleep(200);
        printf("\r  Recording... %ld / %ld frames (%d%%)",
               g_recPos, g_recMax,
               (int)(100.0 * g_recPos / g_recMax));
        fflush(stdout);
    }
    printf("\n");

    g_asio->lpVtbl->stop(g_asio);

    printf("\nCaptured %ld frames (%lld callbacks)\n", g_recPos, g_callbackCount);

    /* Validate */
    validate_buffers(g_recPos);

    /* Write WAV files */
    printf("\n=== Writing WAV files ===\n\n");
    {
        char path[MAX_PATH];
        for (i = 0; i < 2 && i < g_numInputCh; i++) {
            snprintf(path, sizeof(path),
                     "C:\\Users\\elija\\asio_validate_input_ch%ld.wav", i);
            if (write_wav_mono(path, g_recInput[i], g_recPos, SAMPLE_RATE) == 0)
                printf("  Wrote: %s\n", path);
        }
        for (i = 0; i < 2 && i < g_numOutputCh; i++) {
            snprintf(path, sizeof(path),
                     "C:\\Users\\elija\\asio_validate_output_ch%ld.wav", i);
            if (write_wav_mono(path, g_recOutput[i], g_recPos, SAMPLE_RATE) == 0)
                printf("  Wrote: %s\n", path);
        }
    }

cleanup:
    g_asio->lpVtbl->disposeBuffers(g_asio);
    g_asio->lpVtbl->Release(g_asio);
    g_asio = NULL;

    for (i = 0; i < 2; i++) {
        if (g_recInput[i])  free(g_recInput[i]);
        if (g_recOutput[i]) free(g_recOutput[i]);
    }

    CoUninitialize();
    printf("\nDone.\n");
    return 0;
}
