/*
 * asio_loopback.c -- Test/benchmark for HDA Direct ASIO
 *
 * Loads the ASIO driver via COM, creates buffers, runs a mic-to-speaker
 * loopback, and reports latency/xrun statistics.
 *
 * Build (MinGW/w64devkit):
 *   gcc -O2 -Wall -o asio_loopback.exe asio_loopback.c -lole32 -loleaut32
 */

#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <signal.h>

#include "../include/asio.h"

/* ---- Globals ---- */

static volatile int g_running = 1;
static IASIO *g_asio = NULL;
static long g_bufferSize = 0;
static long g_numInputCh = 0;
static long g_numOutputCh = 0;
static volatile long long g_callbackCount = 0;
static volatile long long g_totalFrames = 0;
static LARGE_INTEGER g_startTime;
static LARGE_INTEGER g_perfFreq;

/* ASIO buffer info -- up to 4 channels (2 in + 2 out) */
static ASIOBufferInfo g_bufferInfos[4];

/* ---- Signal handler ---- */

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---- ASIO Callbacks ---- */

static void ASIO_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
    (void)directProcess;

    /*
     * Simple loopback: copy capture buffer to render buffer.
     * Both buffers are in the DMA buffer -- this is a memcpy within
     * physically contiguous memory, as fast as it gets.
     */
    long i;
    for (i = 0; i < g_numInputCh && i < g_numOutputCh; i++) {
        long inIdx = i;             /* Input channel index in g_bufferInfos */
        long outIdx = g_numInputCh + i;  /* Output channel index */

        if (inIdx < 4 && outIdx < 4) {
            void *src = g_bufferInfos[inIdx].buffers[doubleBufferIndex];
            void *dst = g_bufferInfos[outIdx].buffers[doubleBufferIndex];

            if (src && dst) {
                /* Copy interleaved frame data.
                 * For 16-bit stereo at 160 frames: 160 * 4 = 640 bytes per half */
                memcpy(dst, src, g_bufferSize * 4);  /* blockAlign=4 for stereo 16-bit */
            }
        }
    }

    g_callbackCount++;
    g_totalFrames += g_bufferSize;
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
        return 2;  /* ASIO 2.0 */
    default:
        return 0;
    }
}

static ASIOCallbacks g_callbacks = {
    ASIO_bufferSwitch,
    ASIO_sampleRateDidChange,
    ASIO_asioMessage,
    NULL    /* bufferSwitchTimeInfo -- not used */
};

/* ---- Main ---- */

int main(void)
{
    HRESULT hr;
    ASIOError err;
    ASIODriverInfo driverInfo;
    ASIOSampleRate sampleRate;
    long minSize, maxSize, preferredSize, granularity;
    long inputLatency, outputLatency;
    ASIOChannelInfo channelInfo;
    long i;

    printf("=== signal-chain: HDA Direct ASIO Test ===\n\n");

    signal(SIGINT, sigint_handler);

    /* Initialize COM */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx failed: 0x%08lX\n", hr);
        return 1;
    }

    /* Create ASIO driver instance */
    hr = CoCreateInstance(&CLSID_HdaDirectAsio, NULL, CLSCTX_INPROC_SERVER,
                          &CLSID_HdaDirectAsio, (void **)&g_asio);
    if (FAILED(hr)) {
        printf("Cannot create HDA Direct ASIO instance: 0x%08lX\n", hr);
        printf("Is the DLL registered? Run: regsvr32 hda_asio.dll\n");
        CoUninitialize();
        return 1;
    }

    /* Initialize */
    printf("Initializing ASIO driver...\n");
    memset(&driverInfo, 0, sizeof(driverInfo));
    driverInfo.asioVersion = 2;
    driverInfo.sysRef = NULL;

    if (!g_asio->lpVtbl->init(g_asio, NULL)) {
        char errMsg[124];
        g_asio->lpVtbl->getErrorMessage(g_asio, errMsg);
        printf("ASIO init failed: %s\n", errMsg);
        g_asio->lpVtbl->Release(g_asio);
        CoUninitialize();
        return 1;
    }

    /* Query driver info */
    {
        char name[32];
        g_asio->lpVtbl->getDriverName(g_asio, name);
        printf("Driver: %s (v%ld)\n", name, g_asio->lpVtbl->getDriverVersion(g_asio));
    }

    g_asio->lpVtbl->getChannels(g_asio, &g_numInputCh, &g_numOutputCh);
    printf("Channels: %ld in, %ld out\n", g_numInputCh, g_numOutputCh);

    g_asio->lpVtbl->getSampleRate(g_asio, &sampleRate);
    printf("Sample rate: %.0f Hz\n", sampleRate);

    g_asio->lpVtbl->getBufferSize(g_asio, &minSize, &maxSize, &preferredSize, &granularity);
    printf("Buffer size: min=%ld, max=%ld, preferred=%ld, granularity=%ld\n",
           minSize, maxSize, preferredSize, granularity);

    /* Print channel info */
    for (i = 0; i < g_numInputCh; i++) {
        channelInfo.channel = i;
        channelInfo.isInput = ASIOTrue;
        g_asio->lpVtbl->getChannelInfo(g_asio, &channelInfo);
        printf("  Input %ld: %s (type=%ld)\n", i, channelInfo.name, channelInfo.type);
    }
    for (i = 0; i < g_numOutputCh; i++) {
        channelInfo.channel = i;
        channelInfo.isInput = ASIOFalse;
        g_asio->lpVtbl->getChannelInfo(g_asio, &channelInfo);
        printf("  Output %ld: %s (type=%ld)\n", i, channelInfo.name, channelInfo.type);
    }

    /* Create buffers */
    g_bufferSize = preferredSize;
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

    printf("\nCreating ASIO buffers (%ld frames)...\n", g_bufferSize);
    err = g_asio->lpVtbl->createBuffers(g_asio, g_bufferInfos, numChannels,
                                         g_bufferSize, &g_callbacks);
    if (err != ASE_OK) {
        char errMsg[124];
        g_asio->lpVtbl->getErrorMessage(g_asio, errMsg);
        printf("createBuffers failed: %s (error=%ld)\n", errMsg, err);
        g_asio->lpVtbl->Release(g_asio);
        CoUninitialize();
        return 1;
    }

    g_asio->lpVtbl->getLatencies(g_asio, &inputLatency, &outputLatency);

    /* Print configuration summary */
    printf("\n--- Active Configuration ---\n");
    printf("Mode:       ASIO (direct DMA via kernel bridge)\n");
    printf("Format:     16-bit PCM stereo 48kHz\n");
    printf("Buffer:     %ld frames (%.2fms)\n",
           g_bufferSize, (double)g_bufferSize / sampleRate * 1000.0);
    printf("Latency:    in=%ld, out=%ld frames\n", inputLatency, outputLatency);
    printf("Round-trip: %.2fms (buffer only, +hardware ADC/DAC)\n",
           (double)(inputLatency + outputLatency) / sampleRate * 1000.0);

    /* Start streaming */
    printf("\nStarting ASIO...\n");
    QueryPerformanceFrequency(&g_perfFreq);
    QueryPerformanceCounter(&g_startTime);

    err = g_asio->lpVtbl->start(g_asio);
    if (err != ASE_OK) {
        char errMsg[124];
        g_asio->lpVtbl->getErrorMessage(g_asio, errMsg);
        printf("ASIO start failed: %s (error=%ld)\n", errMsg, err);
        g_asio->lpVtbl->disposeBuffers(g_asio);
        g_asio->lpVtbl->Release(g_asio);
        CoUninitialize();
        return 1;
    }

    printf("LIVE -- mic -> speakers [Ctrl+C to stop]\n\n");

    /* Status loop */
    while (g_running) {
        Sleep(1000);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_startTime.QuadPart) / g_perfFreq.QuadPart;

        printf("\r  %.0fs | %lld callbacks | %lld frames",
               elapsed, g_callbackCount, g_totalFrames);
        fflush(stdout);
    }

    printf("\n\nStopping...\n");

    /* Clean shutdown */
    g_asio->lpVtbl->stop(g_asio);
    g_asio->lpVtbl->disposeBuffers(g_asio);
    g_asio->lpVtbl->Release(g_asio);
    g_asio = NULL;

    CoUninitialize();

    printf("Done.\n");
    return 0;
}
