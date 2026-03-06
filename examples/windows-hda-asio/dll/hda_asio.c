/*
 * hda_asio.c -- ASIO DLL for HDA Direct ASIO driver
 *
 * signal-chain experiment: custom ASIO driver for Conexant CX20753/4
 *
 * WASAPI Exclusive Mode backend. Uses event-driven exclusive mode for
 * direct hardware access at minimum latency. The KS direct path was
 * found to be blocked by the Conexant miniport driver (error 22 on
 * state transitions, no RTAUDIO properties), but WASAPI exclusive mode
 * accesses the same DMA hardware through a supported API path.
 *
 * Already proven at 6.67ms round-trip (160 frames @ 48kHz).
 *
 * Build (MinGW/w64devkit):
 *   gcc -shared -O2 -Wall -Wno-unknown-pragmas -I../include \
 *       -o hda_asio.dll ../dll/hda_asio.c ../dll/hda_asio.def \
 *       -lole32 -loleaut32 -lavrt -luuid
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <olectl.h>
#include <stdio.h>
#include <string.h>

/* Our ASIO header */
#include "../include/asio.h"

/* ---- WASAPI GUIDs (defined locally to avoid MinGW link issues) ---- */

DEFINE_GUID(local_CLSID_MMDeviceEnumerator,
    0xBCDE0395, 0xE52F, 0x467C,
    0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(local_IID_IMMDeviceEnumerator,
    0xA95664D2, 0x9614, 0x4F35,
    0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(local_IID_IAudioClient,
    0x1CB9AD4C, 0xDBFA, 0x4C32,
    0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(local_IID_IAudioCaptureClient,
    0xC8ADBD64, 0xE71E, 0x48A0,
    0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(local_IID_IAudioRenderClient,
    0xF294ACFC, 0x3146, 0x4483,
    0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

/* KSDATAFORMAT_SUBTYPE_PCM for WAVEFORMATEXTENSIBLE */
DEFINE_GUID(local_KSDATAFORMAT_SUBTYPE_PCM,
    0x00000001, 0x0000, 0x0010,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

/* ---- Constants that may be missing from MinGW headers ---- */

#ifndef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED  ((HRESULT)0x88890019L)
#endif

#ifndef AUDCLNT_STREAMFLAGS_EVENTCALLBACK
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK  0x00040000
#endif

#ifndef AUDCLNT_BUFFERFLAGS_SILENT
#define AUDCLNT_BUFFERFLAGS_SILENT  0x2
#endif

#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT   0x1
#define SPEAKER_FRONT_RIGHT  0x2
#endif

#ifndef _REFERENCE_TIME_
#define _REFERENCE_TIME_
typedef LONGLONG REFERENCE_TIME;
#endif

/* ---- Configuration ---- */

#define HDA_SAMPLE_RATE         48000
#define HDA_BITS_PER_SAMPLE     16
#define HDA_NUM_CHANNELS        2
#define HDA_BLOCK_ALIGN         (HDA_NUM_CHANNELS * HDA_BITS_PER_SAMPLE / 8)
#define HDA_PREFERRED_FRAMES    160     /* 3.33ms at 48kHz -- HDA codec minimum */

/* Convert frames to REFERENCE_TIME (100-nanosecond units) */
#define FRAMES_TO_REFTIME(frames, rate) \
    ((REFERENCE_TIME)(((LONGLONG)(frames) * 10000000LL) / (rate)))

/* ---- ASIO Driver State ---- */

typedef struct _HDA_ASIO_DRIVER {
    /* COM / IASIO */
    IASIOVtbl      *lpVtbl;
    volatile LONG   refCount;

    /* State */
    BOOL            initialized;
    BOOL            buffersCreated;
    BOOL            running;
    HWND            sysHandle;
    char            errorMessage[124];
    UINT32          currentRate;            /* active sample rate (44100 or 48000) */

    /* WASAPI audio clients (one per direction) */
    IAudioClient        *pCapAudioClient;
    IAudioClient        *pRenAudioClient;

    /* WASAPI streaming sub-clients (acquired at start, released at stop) */
    IAudioCaptureClient *pCapClient;
    IAudioRenderClient  *pRenClient;

    /* Event handles for WASAPI event-driven mode */
    HANDLE          hCaptureEvent;
    HANDLE          hRenderEvent;

    /* Actual buffer sizes from WASAPI (in frames) */
    UINT32          captureFrames;
    UINT32          renderFrames;

    /* ASIO per-channel double-buffers (non-interleaved, as ASIO expects) */
    long            bufferSize;                     /* Frames per half-buffer */
    void           *capCh[HDA_NUM_CHANNELS][2];     /* [channel][half] capture */
    void           *renCh[HDA_NUM_CHANNELS][2];     /* [channel][half] render */
    void           *asioBuffers[2][4];              /* [half][channel] -- up to 2in + 2out */
    long            numInputChannels;
    long            numOutputChannels;
    ASIOCallbacks  *callbacks;

    /* Streaming thread */
    HANDLE          hThread;
    HANDLE          hStopEvent;
    volatile LONG   currentBufferIndex;
    volatile LONGLONG samplePosition;

} HDA_ASIO_DRIVER;

/* ---- Forward declarations ---- */

static HRESULT STDMETHODCALLTYPE Asio_QueryInterface(IASIO *This, REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE Asio_AddRef(IASIO *This);
static ULONG   STDMETHODCALLTYPE Asio_Release(IASIO *This);
static ASIOBool  STDMETHODCALLTYPE Asio_init(IASIO *This, void *sysHandle);
static void      STDMETHODCALLTYPE Asio_getDriverName(IASIO *This, char *name);
static long      STDMETHODCALLTYPE Asio_getDriverVersion(IASIO *This);
static void      STDMETHODCALLTYPE Asio_getErrorMessage(IASIO *This, char *string);
static ASIOError STDMETHODCALLTYPE Asio_start(IASIO *This);
static ASIOError STDMETHODCALLTYPE Asio_stop(IASIO *This);
static ASIOError STDMETHODCALLTYPE Asio_getChannels(IASIO *This, long *numIn, long *numOut);
static ASIOError STDMETHODCALLTYPE Asio_getLatencies(IASIO *This, long *inputLatency, long *outputLatency);
static ASIOError STDMETHODCALLTYPE Asio_getBufferSize(IASIO *This, long *minSize, long *maxSize, long *preferredSize, long *granularity);
static ASIOError STDMETHODCALLTYPE Asio_canSampleRate(IASIO *This, ASIOSampleRate rate);
static ASIOError STDMETHODCALLTYPE Asio_getSampleRate(IASIO *This, ASIOSampleRate *rate);
static ASIOError STDMETHODCALLTYPE Asio_setSampleRate(IASIO *This, ASIOSampleRate rate);
static ASIOError STDMETHODCALLTYPE Asio_getClockSources(IASIO *This, ASIOClockSource *clocks, long *numSources);
static ASIOError STDMETHODCALLTYPE Asio_setClockSource(IASIO *This, long ref);
static ASIOError STDMETHODCALLTYPE Asio_getSamplePosition(IASIO *This, ASIOSamples *sPos, ASIOTimeStamp *tStamp);
static ASIOError STDMETHODCALLTYPE Asio_getChannelInfo(IASIO *This, ASIOChannelInfo *info);
static ASIOError STDMETHODCALLTYPE Asio_createBuffers(IASIO *This, ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks);
static ASIOError STDMETHODCALLTYPE Asio_disposeBuffers(IASIO *This);
static ASIOError STDMETHODCALLTYPE Asio_controlPanel(IASIO *This);
static ASIOError STDMETHODCALLTYPE Asio_future(IASIO *This, long selector, void *opt);
static ASIOError STDMETHODCALLTYPE Asio_outputReady(IASIO *This);

/* ---- Vtable ---- */

static IASIOVtbl g_AsioVtbl = {
    Asio_QueryInterface,
    Asio_AddRef,
    Asio_Release,
    Asio_init,
    Asio_getDriverName,
    Asio_getDriverVersion,
    Asio_getErrorMessage,
    Asio_start,
    Asio_stop,
    Asio_getChannels,
    Asio_getLatencies,
    Asio_getBufferSize,
    Asio_canSampleRate,
    Asio_getSampleRate,
    Asio_setSampleRate,
    Asio_getClockSources,
    Asio_setClockSource,
    Asio_getSamplePosition,
    Asio_getChannelInfo,
    Asio_createBuffers,
    Asio_disposeBuffers,
    Asio_controlPanel,
    Asio_future,
    Asio_outputReady,
};

/* ---- Globals ---- */

static volatile LONG g_dllRefCount = 0;
static HINSTANCE     g_hInstance = NULL;

/* ---- Helper: build our PCM format descriptor ---- */

static void
BuildWaveFormat(WAVEFORMATEXTENSIBLE *pWfx, UINT32 sampleRate)
{
    ZeroMemory(pWfx, sizeof(*pWfx));
    pWfx->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    pWfx->Format.nChannels = HDA_NUM_CHANNELS;
    pWfx->Format.nSamplesPerSec = sampleRate;
    pWfx->Format.wBitsPerSample = HDA_BITS_PER_SAMPLE;
    pWfx->Format.nBlockAlign = HDA_BLOCK_ALIGN;
    pWfx->Format.nAvgBytesPerSec = sampleRate * HDA_BLOCK_ALIGN;
    pWfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    pWfx->Samples.wValidBitsPerSample = HDA_BITS_PER_SAMPLE;
    pWfx->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    pWfx->SubFormat = local_KSDATAFORMAT_SUBTYPE_PCM;
}

/* ---- Helper: initialize one WASAPI exclusive mode client ---- */

static HRESULT
InitWasapiClient(IMMDevice *pDevice, WAVEFORMATEXTENSIBLE *pFmt,
                 IAudioClient **ppClient, HANDLE hEvent, UINT32 *pFrames,
                 const char *label)
{
    IAudioClient *pClient = NULL;
    HRESULT hr;
    REFERENCE_TIME duration;
    UINT32 rate = pFmt->Format.nSamplesPerSec;

    hr = pDevice->lpVtbl->Activate(pDevice, &local_IID_IAudioClient,
                                    CLSCTX_ALL, NULL, (void **)&pClient);
    if (FAILED(hr)) {
        fprintf(stderr, "  [%s] Activate IAudioClient failed: 0x%08lX\n", label, hr);
        return hr;
    }

    /* Request our preferred period */
    duration = FRAMES_TO_REFTIME(HDA_PREFERRED_FRAMES, rate);

    hr = pClient->lpVtbl->Initialize(pClient,
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        duration, duration,
        (WAVEFORMATEX *)pFmt, NULL);

    /* Handle buffer alignment requirement */
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        pClient->lpVtbl->GetBufferSize(pClient, &aligned);
        fprintf(stderr, "  [%s] Buffer not aligned, retrying with %u frames\n",
                label, aligned);
        pClient->lpVtbl->Release(pClient);

        duration = FRAMES_TO_REFTIME(aligned, rate);

        hr = pDevice->lpVtbl->Activate(pDevice, &local_IID_IAudioClient,
                                        CLSCTX_ALL, NULL, (void **)&pClient);
        if (FAILED(hr)) return hr;

        hr = pClient->lpVtbl->Initialize(pClient,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            duration, duration,
            (WAVEFORMATEX *)pFmt, NULL);
    }

    if (FAILED(hr)) {
        fprintf(stderr, "  [%s] Initialize failed: 0x%08lX\n", label, hr);
        pClient->lpVtbl->Release(pClient);
        return hr;
    }

    /* Set event handle */
    hr = pClient->lpVtbl->SetEventHandle(pClient, hEvent);
    if (FAILED(hr)) {
        fprintf(stderr, "  [%s] SetEventHandle failed: 0x%08lX\n", label, hr);
        pClient->lpVtbl->Release(pClient);
        return hr;
    }

    /* Get actual buffer size */
    hr = pClient->lpVtbl->GetBufferSize(pClient, pFrames);
    if (FAILED(hr)) {
        fprintf(stderr, "  [%s] GetBufferSize failed: 0x%08lX\n", label, hr);
        pClient->lpVtbl->Release(pClient);
        return hr;
    }

    fprintf(stderr, "  [%s] WASAPI exclusive: %u frames (%.2fms @ %uHz)\n",
            label, *pFrames, (double)*pFrames / rate * 1000.0, rate);

    *ppClient = pClient;
    return S_OK;
}

/* ---- Streaming thread ---- */

static DWORD WINAPI
StreamingThread(LPVOID param)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)param;
    HANDLE waitHandles[2];
    DWORD taskIndex = 0;
    HANDLE hTask;
    long bufferIndex = 0;
    HRESULT hr;

    waitHandles[0] = drv->hStopEvent;
    waitHandles[1] = drv->hCaptureEvent;

    /* Boost thread priority via MMCSS */
    hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (1) {
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);

        if (wait == WAIT_OBJECT_0) break;        /* Stop event signaled */
        if (wait != WAIT_OBJECT_0 + 1) continue; /* Timeout or error */

        /* --- Capture: read interleaved WASAPI -> deinterleave to per-channel ASIO buffers --- */
        {
            BYTE *pData = NULL;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = drv->pCapClient->lpVtbl->GetBuffer(drv->pCapClient,
                &pData, &numFrames, &flags, NULL, NULL);
            if (SUCCEEDED(hr) && numFrames > 0) {
                UINT32 copyFrames = numFrames;
                if ((long)copyFrames > drv->bufferSize)
                    copyFrames = (UINT32)drv->bufferSize;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    int ch;
                    for (ch = 0; ch < HDA_NUM_CHANNELS; ch++)
                        ZeroMemory(drv->capCh[ch][bufferIndex],
                                   copyFrames * (HDA_BITS_PER_SAMPLE / 8));
                } else {
                    /* Deinterleave: [L0,R0,L1,R1,...] -> L[0..N], R[0..N] */
                    short *interleaved = (short *)pData;
                    short *left  = (short *)drv->capCh[0][bufferIndex];
                    short *right = (short *)drv->capCh[1][bufferIndex];
                    UINT32 f;
                    for (f = 0; f < copyFrames; f++) {
                        left[f]  = interleaved[f * 2];
                        right[f] = interleaved[f * 2 + 1];
                    }
                }

                drv->pCapClient->lpVtbl->ReleaseBuffer(drv->pCapClient, numFrames);
            } else if (SUCCEEDED(hr)) {
                drv->pCapClient->lpVtbl->ReleaseBuffer(drv->pCapClient, 0);
                continue;
            } else {
                continue;
            }
        }

        /* --- Fire ASIO callback --- */
        InterlockedExchange(&drv->currentBufferIndex, bufferIndex);
        if (drv->callbacks && drv->callbacks->bufferSwitch) {
            drv->callbacks->bufferSwitch(bufferIndex, ASIOTrue);
        }

        /* --- Render: reinterleave from per-channel ASIO buffers -> WASAPI --- */
        {
            UINT32 renFrames = drv->renderFrames;
            if ((long)renFrames > drv->bufferSize)
                renFrames = (UINT32)drv->bufferSize;

            BYTE *pData = NULL;
            hr = drv->pRenClient->lpVtbl->GetBuffer(drv->pRenClient,
                renFrames, &pData);
            if (SUCCEEDED(hr)) {
                /* Reinterleave: L[0..N], R[0..N] -> [L0,R0,L1,R1,...] */
                short *interleaved = (short *)pData;
                short *left  = (short *)drv->renCh[0][bufferIndex];
                short *right = (short *)drv->renCh[1][bufferIndex];
                UINT32 f;
                for (f = 0; f < renFrames; f++) {
                    interleaved[f * 2]     = left[f];
                    interleaved[f * 2 + 1] = right[f];
                }
                drv->pRenClient->lpVtbl->ReleaseBuffer(drv->pRenClient,
                    renFrames, 0);
            }
        }

        /* Track position */
        drv->samplePosition += drv->captureFrames;

        /* Swap double buffer */
        bufferIndex = 1 - bufferIndex;
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    return 0;
}

/* ---- IUnknown implementation ---- */

static HRESULT STDMETHODCALLTYPE
Asio_QueryInterface(IASIO *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &CLSID_HdaDirectAsio)) {
        *ppv = This;
        Asio_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
Asio_AddRef(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    return InterlockedIncrement(&drv->refCount);
}

static ULONG STDMETHODCALLTYPE
Asio_Release(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    LONG ref = InterlockedDecrement(&drv->refCount);
    if (ref == 0) {
        if (drv->running) Asio_stop(This);
        if (drv->buffersCreated) Asio_disposeBuffers(This);

        if (drv->hCaptureEvent) { CloseHandle(drv->hCaptureEvent); drv->hCaptureEvent = NULL; }
        if (drv->hRenderEvent) { CloseHandle(drv->hRenderEvent); drv->hRenderEvent = NULL; }
        if (drv->pCapAudioClient) { drv->pCapAudioClient->lpVtbl->Release(drv->pCapAudioClient); drv->pCapAudioClient = NULL; }
        if (drv->pRenAudioClient) { drv->pRenAudioClient->lpVtbl->Release(drv->pRenAudioClient); drv->pRenAudioClient = NULL; }

        free(drv);
        InterlockedDecrement(&g_dllRefCount);
    }
    return ref;
}

/* ---- IASIO implementation ---- */

static ASIOBool STDMETHODCALLTYPE
Asio_init(IASIO *This, void *sysHandle)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    HRESULT hr;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pCapDev = NULL, *pRenDev = NULL;
    WAVEFORMATEXTENSIBLE wfx;

    if (drv->initialized) return ASIOTrue;

    /* Default to 48kHz if no rate set yet */
    if (drv->currentRate == 0) drv->currentRate = HDA_SAMPLE_RATE;

    drv->sysHandle = (HWND)sysHandle;
    drv->numInputChannels = HDA_NUM_CHANNELS;
    drv->numOutputChannels = HDA_NUM_CHANNELS;

    /* Create device enumerator */
    hr = CoCreateInstance(&local_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &local_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot create MMDeviceEnumerator (0x%08lX)", hr);
        return ASIOFalse;
    }

    /* Get default capture endpoint */
    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eCapture, eConsole, &pCapDev);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "No capture device (0x%08lX)", hr);
        pEnum->lpVtbl->Release(pEnum);
        return ASIOFalse;
    }

    /* Get default render endpoint */
    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pRenDev);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "No render device (0x%08lX)", hr);
        pCapDev->lpVtbl->Release(pCapDev);
        pEnum->lpVtbl->Release(pEnum);
        return ASIOFalse;
    }

    pEnum->lpVtbl->Release(pEnum);
    pEnum = NULL;

    /* Log device IDs */
    {
        LPWSTR capId = NULL, renId = NULL;
        if (SUCCEEDED(pCapDev->lpVtbl->GetId(pCapDev, &capId))) {
            fprintf(stderr, "  Capture device: %ls\n", capId);
            CoTaskMemFree(capId);
        }
        if (SUCCEEDED(pRenDev->lpVtbl->GetId(pRenDev, &renId))) {
            fprintf(stderr, "  Render device:  %ls\n", renId);
            CoTaskMemFree(renId);
        }
    }

    /* Build our format descriptor */
    BuildWaveFormat(&wfx, drv->currentRate);

    /* Create event handles */
    drv->hCaptureEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    drv->hRenderEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!drv->hCaptureEvent || !drv->hRenderEvent) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot create event handles");
        pCapDev->lpVtbl->Release(pCapDev);
        pRenDev->lpVtbl->Release(pRenDev);
        return ASIOFalse;
    }

    /* Initialize capture client */
    hr = InitWasapiClient(pCapDev, &wfx, &drv->pCapAudioClient,
                          drv->hCaptureEvent, &drv->captureFrames, "capture");
    pCapDev->lpVtbl->Release(pCapDev);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Capture init failed (0x%08lX)", hr);
        pRenDev->lpVtbl->Release(pRenDev);
        return ASIOFalse;
    }

    /* Initialize render client */
    hr = InitWasapiClient(pRenDev, &wfx, &drv->pRenAudioClient,
                          drv->hRenderEvent, &drv->renderFrames, "render");
    pRenDev->lpVtbl->Release(pRenDev);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Render init failed (0x%08lX)", hr);
        drv->pCapAudioClient->lpVtbl->Release(drv->pCapAudioClient);
        drv->pCapAudioClient = NULL;
        return ASIOFalse;
    }

    fprintf(stderr, "  WASAPI exclusive mode initialized @ %u Hz\n", drv->currentRate);
    fprintf(stderr, "  Capture: %u frames (%.2fms)\n",
            drv->captureFrames,
            (double)drv->captureFrames / drv->currentRate * 1000.0);
    fprintf(stderr, "  Render:  %u frames (%.2fms)\n",
            drv->renderFrames,
            (double)drv->renderFrames / drv->currentRate * 1000.0);

    drv->initialized = TRUE;
    return ASIOTrue;
}

static void STDMETHODCALLTYPE
Asio_getDriverName(IASIO *This, char *name)
{
    (void)This;
    strcpy(name, HDA_ASIO_DRIVER_NAME);
}

static long STDMETHODCALLTYPE
Asio_getDriverVersion(IASIO *This)
{
    (void)This;
    return 2;
}

static void STDMETHODCALLTYPE
Asio_getErrorMessage(IASIO *This, char *string)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    strcpy(string, drv->errorMessage);
}

static ASIOError STDMETHODCALLTYPE
Asio_start(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    HRESULT hr;

    if (!drv->buffersCreated) return ASE_InvalidMode;
    if (drv->running) return ASE_OK;

    /* Get streaming sub-clients */
    hr = drv->pCapAudioClient->lpVtbl->GetService(drv->pCapAudioClient,
        &local_IID_IAudioCaptureClient, (void **)&drv->pCapClient);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot get capture service (0x%08lX)", hr);
        return ASE_HWMalfunction;
    }

    hr = drv->pRenAudioClient->lpVtbl->GetService(drv->pRenAudioClient,
        &local_IID_IAudioRenderClient, (void **)&drv->pRenClient);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot get render service (0x%08lX)", hr);
        drv->pCapClient->lpVtbl->Release(drv->pCapClient);
        drv->pCapClient = NULL;
        return ASE_HWMalfunction;
    }

    /* Pre-fill render buffer with silence */
    {
        BYTE *pSilence = NULL;
        hr = drv->pRenClient->lpVtbl->GetBuffer(drv->pRenClient,
            drv->renderFrames, &pSilence);
        if (SUCCEEDED(hr)) {
            ZeroMemory(pSilence, drv->renderFrames * HDA_BLOCK_ALIGN);
            drv->pRenClient->lpVtbl->ReleaseBuffer(drv->pRenClient,
                drv->renderFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    /* Create stop event and streaming thread */
    drv->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    drv->currentBufferIndex = 0;
    drv->samplePosition = 0;

    drv->hThread = CreateThread(NULL, 0, StreamingThread, drv, 0, NULL);
    if (!drv->hThread) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot create streaming thread");
        drv->pCapClient->lpVtbl->Release(drv->pCapClient); drv->pCapClient = NULL;
        drv->pRenClient->lpVtbl->Release(drv->pRenClient); drv->pRenClient = NULL;
        CloseHandle(drv->hStopEvent); drv->hStopEvent = NULL;
        return ASE_HWMalfunction;
    }

    /* Start render first (has pre-filled silence), then capture */
    hr = drv->pRenAudioClient->lpVtbl->Start(drv->pRenAudioClient);
    if (FAILED(hr)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Render start failed (0x%08lX)", hr);
        goto start_fail;
    }

    hr = drv->pCapAudioClient->lpVtbl->Start(drv->pCapAudioClient);
    if (FAILED(hr)) {
        drv->pRenAudioClient->lpVtbl->Stop(drv->pRenAudioClient);
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Capture start failed (0x%08lX)", hr);
        goto start_fail;
    }

    drv->running = TRUE;
    fprintf(stderr, "  ASIO started (WASAPI exclusive, %u frames, %.2fms @ %uHz)\n",
            drv->captureFrames,
            (double)drv->captureFrames / drv->currentRate * 1000.0,
            drv->currentRate);
    return ASE_OK;

start_fail:
    SetEvent(drv->hStopEvent);
    WaitForSingleObject(drv->hThread, 5000);
    CloseHandle(drv->hThread); drv->hThread = NULL;
    CloseHandle(drv->hStopEvent); drv->hStopEvent = NULL;
    drv->pCapClient->lpVtbl->Release(drv->pCapClient); drv->pCapClient = NULL;
    drv->pRenClient->lpVtbl->Release(drv->pRenClient); drv->pRenClient = NULL;
    return ASE_HWMalfunction;
}

static ASIOError STDMETHODCALLTYPE
Asio_stop(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;

    if (!drv->running) return ASE_OK;

    /* Signal thread to stop and wait */
    SetEvent(drv->hStopEvent);
    WaitForSingleObject(drv->hThread, 5000);
    CloseHandle(drv->hThread);
    CloseHandle(drv->hStopEvent);
    drv->hThread = NULL;
    drv->hStopEvent = NULL;

    /* Stop both audio clients */
    if (drv->pCapAudioClient) drv->pCapAudioClient->lpVtbl->Stop(drv->pCapAudioClient);
    if (drv->pRenAudioClient) drv->pRenAudioClient->lpVtbl->Stop(drv->pRenAudioClient);

    /* Release streaming sub-clients */
    if (drv->pCapClient) { drv->pCapClient->lpVtbl->Release(drv->pCapClient); drv->pCapClient = NULL; }
    if (drv->pRenClient) { drv->pRenClient->lpVtbl->Release(drv->pRenClient); drv->pRenClient = NULL; }

    drv->running = FALSE;
    fprintf(stderr, "  ASIO stopped\n");
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getChannels(IASIO *This, long *numIn, long *numOut)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    *numIn = drv->numInputChannels;
    *numOut = drv->numOutputChannels;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getLatencies(IASIO *This, long *inputLatency, long *outputLatency)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    long frames = (drv->captureFrames > 0) ? (long)drv->captureFrames : HDA_PREFERRED_FRAMES;
    *inputLatency = frames;
    *outputLatency = frames;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getBufferSize(IASIO *This, long *minSize, long *maxSize,
                   long *preferredSize, long *granularity)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    long frames = (drv->captureFrames > 0) ? (long)drv->captureFrames : HDA_PREFERRED_FRAMES;
    /* Fixed buffer size in WASAPI exclusive mode */
    *minSize = frames;
    *maxSize = frames;
    *preferredSize = frames;
    *granularity = 0;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_canSampleRate(IASIO *This, ASIOSampleRate rate)
{
    (void)This;
    UINT32 r = (UINT32)rate;
    if (r == 44100 || r == 48000) return ASE_OK;
    return ASE_NoClock;
}

static ASIOError STDMETHODCALLTYPE
Asio_getSampleRate(IASIO *This, ASIOSampleRate *rate)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    *rate = (ASIOSampleRate)(drv->currentRate ? drv->currentRate : HDA_SAMPLE_RATE);
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_setSampleRate(IASIO *This, ASIOSampleRate rate)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    UINT32 newRate = (UINT32)rate;

    if (newRate != 44100 && newRate != 48000) return ASE_NoClock;
    if (newRate == drv->currentRate) return ASE_OK;

    /* Tear down current WASAPI state for re-init at new rate */
    if (drv->running) Asio_stop(This);
    if (drv->buffersCreated) Asio_disposeBuffers(This);

    if (drv->pCapAudioClient) {
        drv->pCapAudioClient->lpVtbl->Release(drv->pCapAudioClient);
        drv->pCapAudioClient = NULL;
    }
    if (drv->pRenAudioClient) {
        drv->pRenAudioClient->lpVtbl->Release(drv->pRenAudioClient);
        drv->pRenAudioClient = NULL;
    }
    if (drv->hCaptureEvent) { CloseHandle(drv->hCaptureEvent); drv->hCaptureEvent = NULL; }
    if (drv->hRenderEvent)  { CloseHandle(drv->hRenderEvent);  drv->hRenderEvent = NULL; }

    drv->currentRate = newRate;
    drv->initialized = FALSE;

    fprintf(stderr, "  setSampleRate: switching to %u Hz\n", newRate);

    /* Re-initialize WASAPI with new rate */
    if (!Asio_init(This, drv->sysHandle)) return ASE_HWMalfunction;

    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getClockSources(IASIO *This, ASIOClockSource *clocks, long *numSources)
{
    (void)This;
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy(clocks[0].name, "Internal");
    *numSources = 1;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_setClockSource(IASIO *This, long ref)
{
    (void)This;
    if (ref != 0) return ASE_InvalidParameter;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getSamplePosition(IASIO *This, ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    LARGE_INTEGER pc, freq;

    *sPos = (ASIOSamples)drv->samplePosition;

    QueryPerformanceCounter(&pc);
    QueryPerformanceFrequency(&freq);
    *tStamp = (ASIOTimeStamp)((pc.QuadPart * 1000000000LL) / freq.QuadPart);

    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getChannelInfo(IASIO *This, ASIOChannelInfo *info)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;

    if (info->isInput) {
        if (info->channel >= drv->numInputChannels) return ASE_InvalidParameter;
        info->type = ASIOSTInt16LSB;
        info->channelGroup = 0;
        snprintf(info->name, sizeof(info->name), "Capture %ld", info->channel + 1);
    } else {
        if (info->channel >= drv->numOutputChannels) return ASE_InvalidParameter;
        info->type = ASIOSTInt16LSB;
        info->channelGroup = 0;
        snprintf(info->name, sizeof(info->name), "Render %ld", info->channel + 1);
    }

    info->isActive = drv->buffersCreated ? ASIOTrue : ASIOFalse;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_createBuffers(IASIO *This, ASIOBufferInfo *bufferInfos, long numChannels,
                   long bufferSize, ASIOCallbacks *callbacks)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;
    long i;
    ULONG bufBytes;

    if (!drv->initialized) return ASE_NotPresent;
    if (drv->buffersCreated) Asio_disposeBuffers(This);

    drv->bufferSize = bufferSize;
    drv->callbacks = callbacks;

    /*
     * Allocate per-channel non-interleaved double buffers.
     * ASIO expects each channel to have its own contiguous buffer of
     * mono samples: [S0, S1, S2, ...] with stride = sizeof(sample).
     * WASAPI provides/expects interleaved stereo, so we deinterleave
     * on capture and reinterleave on render in the streaming thread.
     */
    bufBytes = bufferSize * (HDA_BITS_PER_SAMPLE / 8);  /* per-channel size in bytes */

    {
        int ch;
        for (ch = 0; ch < HDA_NUM_CHANNELS; ch++) {
            drv->capCh[ch][0] = VirtualAlloc(NULL, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            drv->capCh[ch][1] = VirtualAlloc(NULL, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            drv->renCh[ch][0] = VirtualAlloc(NULL, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            drv->renCh[ch][1] = VirtualAlloc(NULL, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!drv->capCh[ch][0] || !drv->capCh[ch][1] ||
                !drv->renCh[ch][0] || !drv->renCh[ch][1]) {
                snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                         "Cannot allocate ASIO buffers");
                return ASE_NoMemory;
            }
        }
    }

    /* Point ASIO buffer pointers to per-channel non-interleaved buffers */
    for (i = 0; i < numChannels; i++) {
        long ch = bufferInfos[i].channelIndex;

        if (bufferInfos[i].isInput) {
            bufferInfos[i].buffers[0] = drv->capCh[ch][0];
            bufferInfos[i].buffers[1] = drv->capCh[ch][1];
        } else {
            bufferInfos[i].buffers[0] = drv->renCh[ch][0];
            bufferInfos[i].buffers[1] = drv->renCh[ch][1];
        }

        if (i < 4) {
            drv->asioBuffers[0][i] = bufferInfos[i].buffers[0];
            drv->asioBuffers[1][i] = bufferInfos[i].buffers[1];
        }
    }

    drv->buffersCreated = TRUE;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_disposeBuffers(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;

    if (drv->running) Asio_stop(This);

    {
        int ch;
        for (ch = 0; ch < HDA_NUM_CHANNELS; ch++) {
            if (drv->capCh[ch][0]) { VirtualFree(drv->capCh[ch][0], 0, MEM_RELEASE); drv->capCh[ch][0] = NULL; }
            if (drv->capCh[ch][1]) { VirtualFree(drv->capCh[ch][1], 0, MEM_RELEASE); drv->capCh[ch][1] = NULL; }
            if (drv->renCh[ch][0]) { VirtualFree(drv->renCh[ch][0], 0, MEM_RELEASE); drv->renCh[ch][0] = NULL; }
            if (drv->renCh[ch][1]) { VirtualFree(drv->renCh[ch][1], 0, MEM_RELEASE); drv->renCh[ch][1] = NULL; }
        }
    }

    drv->buffersCreated = FALSE;
    drv->callbacks = NULL;
    ZeroMemory(drv->asioBuffers, sizeof(drv->asioBuffers));
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_controlPanel(IASIO *This)
{
    (void)This;
    MessageBoxW(NULL,
        L"HDA Direct ASIO\n\n"
        L"WASAPI Exclusive Mode ASIO driver\n"
        L"for Conexant CX20753/4 on Intel HDA bus.\n\n"
        L"Configuration:\n"
        L"  44100 / 48000 Hz / 16-bit / Stereo\n"
        L"  Event-driven exclusive mode\n\n"
        L"signal-chain experiment\n"
        L"github.com/ELI7VH/signal-chain",
        L"HDA Direct ASIO",
        MB_OK | MB_ICONINFORMATION);
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_future(IASIO *This, long selector, void *opt)
{
    (void)This;
    (void)opt;

    switch (selector) {
    case kAsioSupportsTimeInfo:
        return ASE_SUCCESS;
    case kAsioSupportsTimeCode:
        return ASE_SUCCESS;
    default:
        return ASE_InvalidParameter;
    }
}

static ASIOError STDMETHODCALLTYPE
Asio_outputReady(IASIO *This)
{
    (void)This;
    /* Return ASE_NotPresent -- we don't support the outputReady optimization.
     * Our streaming thread pushes render data on its own schedule.
     * Returning ASE_OK here lies to the host about timing, which causes
     * DAWs like Studio One to report 100% CPU usage. */
    return ASE_NotPresent;
}

/* ---- COM Class Factory ---- */

typedef struct {
    IClassFactoryVtbl *lpVtbl;
    volatile LONG refCount;
} HDA_CLASS_FACTORY;

static HRESULT STDMETHODCALLTYPE CF_QueryInterface(IClassFactory *This, REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE CF_AddRef(IClassFactory *This);
static ULONG   STDMETHODCALLTYPE CF_Release(IClassFactory *This);
static HRESULT STDMETHODCALLTYPE CF_CreateInstance(IClassFactory *This, IUnknown *pUnk, REFIID riid, void **ppv);
static HRESULT STDMETHODCALLTYPE CF_LockServer(IClassFactory *This, BOOL fLock);

static IClassFactoryVtbl g_ClassFactoryVtbl = {
    CF_QueryInterface,
    CF_AddRef,
    CF_Release,
    CF_CreateInstance,
    CF_LockServer,
};

static HDA_CLASS_FACTORY g_ClassFactory = { &g_ClassFactoryVtbl, 1 };

static HRESULT STDMETHODCALLTYPE
CF_QueryInterface(IClassFactory *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        CF_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CF_AddRef(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE CF_Release(IClassFactory *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE
CF_CreateInstance(IClassFactory *This, IUnknown *pUnk, REFIID riid, void **ppv)
{
    HDA_ASIO_DRIVER *drv;
    (void)This;

    if (pUnk) return CLASS_E_NOAGGREGATION;

    drv = (HDA_ASIO_DRIVER *)calloc(1, sizeof(HDA_ASIO_DRIVER));
    if (!drv) return E_OUTOFMEMORY;

    drv->lpVtbl = &g_AsioVtbl;
    drv->refCount = 1;
    drv->currentRate = HDA_SAMPLE_RATE;  /* default 48kHz */
    InterlockedIncrement(&g_dllRefCount);

    *ppv = (void *)drv;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE
CF_LockServer(IClassFactory *This, BOOL fLock)
{
    (void)This;
    if (fLock) InterlockedIncrement(&g_dllRefCount);
    else InterlockedDecrement(&g_dllRefCount);
    return S_OK;
}

/* ---- DLL Exports ---- */

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if (IsEqualCLSID(rclsid, &CLSID_HdaDirectAsio)) {
        return CF_QueryInterface((IClassFactory *)&g_ClassFactory, riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return (g_dllRefCount == 0) ? S_OK : S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    HKEY hKey;
    LONG res;
    char dllPath[MAX_PATH];
    char clsidPath[256];

    GetModuleFileNameA(g_hInstance, dllPath, MAX_PATH);

    /* Register COM InprocServer32 */
    snprintf(clsidPath, sizeof(clsidPath),
             "CLSID\\%s\\InprocServer32", HDA_ASIO_CLSID_STR);

    res = RegCreateKeyExA(HKEY_CLASSES_ROOT, clsidPath, 0, NULL,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (res != ERROR_SUCCESS) return SELFREG_E_CLASS;

    RegSetValueExA(hKey, NULL, 0, REG_SZ, (BYTE *)dllPath, (DWORD)strlen(dllPath) + 1);
    RegSetValueExA(hKey, "ThreadingModel", 0, REG_SZ, (BYTE *)"Both", 5);
    RegCloseKey(hKey);

    /* Set CLSID display name */
    snprintf(clsidPath, sizeof(clsidPath), "CLSID\\%s", HDA_ASIO_CLSID_STR);
    res = RegOpenKeyExA(HKEY_CLASSES_ROOT, clsidPath, 0, KEY_WRITE, &hKey);
    if (res == ERROR_SUCCESS) {
        RegSetValueExA(hKey, NULL, 0, REG_SZ,
                       (BYTE *)HDA_ASIO_DRIVER_NAME,
                       (DWORD)strlen(HDA_ASIO_DRIVER_NAME) + 1);
        RegCloseKey(hKey);
    }

    /* Register ASIO driver */
    snprintf(clsidPath, sizeof(clsidPath),
             "SOFTWARE\\ASIO\\%s", HDA_ASIO_DRIVER_NAME);

    res = RegCreateKeyExA(HKEY_LOCAL_MACHINE, clsidPath, 0, NULL,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (res != ERROR_SUCCESS) return SELFREG_E_CLASS;

    RegSetValueExA(hKey, "CLSID", 0, REG_SZ,
                   (BYTE *)HDA_ASIO_CLSID_STR,
                   (DWORD)strlen(HDA_ASIO_CLSID_STR) + 1);
    RegSetValueExA(hKey, "Description", 0, REG_SZ,
                   (BYTE *)"Direct HDA ASIO - WASAPI exclusive",
                   35);
    RegCloseKey(hKey);

    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    char path[256];

    /* Remove ASIO registration */
    snprintf(path, sizeof(path), "SOFTWARE\\ASIO\\%s", HDA_ASIO_DRIVER_NAME);
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, path);

    /* Remove COM registration */
    snprintf(path, sizeof(path), "CLSID\\%s\\InprocServer32", HDA_ASIO_CLSID_STR);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, path);

    snprintf(path, sizeof(path), "CLSID\\%s", HDA_ASIO_CLSID_STR);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, path);

    return S_OK;
}
