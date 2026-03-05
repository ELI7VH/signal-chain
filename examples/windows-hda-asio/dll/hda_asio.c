/*
 * hda_asio.c -- ASIO DLL for HDA Direct ASIO driver
 *
 * signal-chain experiment: custom ASIO driver for Conexant CX20753/4
 *
 * This is a COM in-process server that implements the ASIO interface.
 * It creates KS pins directly on the HDA wave filter (user-mode operation),
 * then uses the hda_bridge.sys kernel driver to allocate WaveRT DMA buffers
 * (the one operation requiring kernel mode).
 *
 * The hot path (bufferSwitch callback) runs entirely in user mode:
 *   - Read capture samples from DMA buffer (zero-copy)
 *   - Write render samples to DMA buffer (zero-copy)
 *   - Read hardware position register (zero-syscall)
 *
 * Build (MinGW/w64devkit):
 *   gcc -shared -O2 -Wall -o hda_asio.dll hda_asio.c hda_asio.def
 *       -lole32 -loleaut32 -lsetupapi -lksuser -lavrt -luuid
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID

#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <olectl.h>
#include <stdio.h>
#include <string.h>

/* Our headers */
#include "../include/asio.h"
#include "../include/hda_bridge_ioctl.h"

/* ---- KS GUIDs (from ksuser.lib) ---- */

/* These are already in ksuser.lib, declare as extern */
extern const GUID KSCATEGORY_AUDIO;
extern const GUID KSCATEGORY_RENDER;
extern const GUID KSCATEGORY_CAPTURE;
extern const GUID KSPROPSETID_Pin;
extern const GUID KSPROPSETID_Connection;
extern const GUID KSDATAFORMAT_TYPE_AUDIO;
extern const GUID KSDATAFORMAT_SUBTYPE_PCM;
extern const GUID KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

/* Interface GUIDs -- define locally */
DEFINE_GUID(LOCAL_KSINTERFACESETID_Standard,
    0x1A8766A0L, 0x62CE, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00);
DEFINE_GUID(LOCAL_KSMEDIUMSETID_Standard,
    0x4747B320L, 0x62CE, 0x11CF, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00);

/* KS interface IDs */
#ifndef KSINTERFACE_STANDARD_LOOPED_STREAMING
#define KSINTERFACE_STANDARD_LOOPED_STREAMING 2
#endif

/* KS IOCTLs (may not be in w64devkit headers) */
#ifndef FILE_DEVICE_KS
#define FILE_DEVICE_KS 0x0000002F
#endif
#ifndef IOCTL_KS_PROPERTY
#include <winioctl.h>
#define IOCTL_KS_PROPERTY CTL_CODE(FILE_DEVICE_KS, 0x000, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

/* ---- Configuration ---- */

#define HDA_SAMPLE_RATE         48000
#define HDA_BITS_PER_SAMPLE     16
#define HDA_NUM_CHANNELS        2
#define HDA_BLOCK_ALIGN         (HDA_NUM_CHANNELS * HDA_BITS_PER_SAMPLE / 8)
#define HDA_PREFERRED_FRAMES    160     /* 3.33ms at 48kHz -- HDA codec minimum */
#define HDA_BYTES_PER_BUFFER    (HDA_PREFERRED_FRAMES * HDA_BLOCK_ALIGN)

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

    /* Kernel bridge */
    HANDLE          hBridge;        /* Handle to \\.\HdaAsioBridge */

    /* KS handles */
    HANDLE          hCaptureFilter; /* HDA capture wave filter */
    HANDLE          hRenderFilter;  /* HDA render wave filter */
    HANDLE          hCapturePin;    /* KS capture pin */
    HANDLE          hRenderPin;     /* KS render pin */

    /* Device paths (found during enumeration) */
    WCHAR           captureFilterPath[512];
    WCHAR           renderFilterPath[512];

    /* DMA buffers (mapped by kernel bridge) */
    void           *captureBuffer;      /* DMA buffer for capture */
    ULONG           captureBufferSize;
    void           *renderBuffer;       /* DMA buffer for render */
    ULONG           renderBufferSize;

    /* Position registers (mapped by kernel bridge, may be NULL) */
    volatile ULONG *capturePosition;
    volatile ULONG *renderPosition;

    /* ASIO double-buffering */
    long            bufferSize;         /* Frames per half-buffer */
    void           *asioBuffers[2][4];  /* [half][channel] -- up to 2in + 2out */
    long            numInputChannels;
    long            numOutputChannels;
    ASIOCallbacks  *callbacks;

    /* Streaming thread */
    HANDLE          hThread;
    HANDLE          hStopEvent;
    volatile LONG   currentBufferIndex;

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

/* ---- Utility: find HDA wave filter by category ---- */

static BOOL
FindHdaWaveFilter(const GUID *category, WCHAR *pathOut, DWORD pathOutChars)
{
    HDEVINFO devInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = NULL;
    DWORD needed;
    BOOL found = FALSE;
    DWORD idx;

    devInfo = SetupDiGetClassDevsW(category, NULL, NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return FALSE;

    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, category, idx, &ifData); idx++) {
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(needed);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL)) {
            /* Look for HDA wave filter (not topology, not software engine) */
            WCHAR *path = detail->DevicePath;
            BOOL isHda = (wcsstr(path, L"hdaudio") != NULL);
            BOOL isWave = (wcsstr(path, L"wave") != NULL);

            if (isHda && isWave) {
                wcsncpy(pathOut, path, pathOutChars - 1);
                pathOut[pathOutChars - 1] = L'\0';
                found = TRUE;
                free(detail);
                break;
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

/* ---- Utility: create a WaveRT KS pin ---- */

static HANDLE
CreateWaveRtPin(HANDLE hFilter, DWORD pinId, BOOL isCapture)
{
    /*
     * Build KSPIN_CONNECT + KSDATAFORMAT_WAVEFORMATEX for WaveRT looped streaming.
     * This is the same technique proven in our KS passthrough experiment.
     */
    struct {
        KSPIN_CONNECT           Connect;
        KSDATAFORMAT_WAVEFORMATEX DataFormat;
    } pinRequest;

    HANDLE hPin = NULL;
    DWORD err;

    ZeroMemory(&pinRequest, sizeof(pinRequest));

    /* Pin connect header */
    pinRequest.Connect.Interface.Set = LOCAL_KSINTERFACESETID_Standard;
    pinRequest.Connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
    pinRequest.Connect.Medium.Set = LOCAL_KSMEDIUMSETID_Standard;
    pinRequest.Connect.Medium.Id = 0;  /* KSMEDIUM_STANDARD_DEVIO */
    pinRequest.Connect.PinId = pinId;
    pinRequest.Connect.PinToHandle = NULL;
    pinRequest.Connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
    pinRequest.Connect.Priority.PrioritySubClass = KSPRIORITY_NORMAL;

    /* Data format: PCM stereo 48kHz 16-bit */
    pinRequest.DataFormat.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    pinRequest.DataFormat.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    pinRequest.DataFormat.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    pinRequest.DataFormat.DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    pinRequest.DataFormat.DataFormat.SampleSize = HDA_BLOCK_ALIGN;

    pinRequest.DataFormat.WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    pinRequest.DataFormat.WaveFormatEx.nChannels = HDA_NUM_CHANNELS;
    pinRequest.DataFormat.WaveFormatEx.nSamplesPerSec = HDA_SAMPLE_RATE;
    pinRequest.DataFormat.WaveFormatEx.wBitsPerSample = HDA_BITS_PER_SAMPLE;
    pinRequest.DataFormat.WaveFormatEx.nBlockAlign = HDA_BLOCK_ALIGN;
    pinRequest.DataFormat.WaveFormatEx.nAvgBytesPerSec = HDA_SAMPLE_RATE * HDA_BLOCK_ALIGN;
    pinRequest.DataFormat.WaveFormatEx.cbSize = 0;

    err = KsCreatePin(hFilter, &pinRequest.Connect, GENERIC_READ | GENERIC_WRITE, &hPin);
    if (err != ERROR_SUCCESS) {
        return NULL;
    }
    return hPin;
}

/* ---- Utility: bridge IOCTL helpers ---- */

static BOOL
BridgeAllocBuffer(HANDLE hBridge, HANDLE hPin, ULONG requestedSize,
                  void **bufferOut, ULONG *sizeOut)
{
    HDA_ALLOC_BUFFER_IN in;
    HDA_ALLOC_BUFFER_OUT out;
    DWORD bytesReturned;

    in.PinHandle = (ULONG_PTR)hPin;
    in.RequestedSize = requestedSize;

    if (!DeviceIoControl(hBridge, IOCTL_HDA_ALLOC_RT_BUFFER,
            &in, sizeof(in), &out, sizeof(out), &bytesReturned, NULL)) {
        return FALSE;
    }

    *bufferOut = (void *)out.BufferAddress;
    *sizeOut = out.BufferSize;
    return TRUE;
}

static BOOL
BridgeFreeBuffer(HANDLE hBridge, HANDLE hPin)
{
    HDA_FREE_BUFFER_IN in;
    DWORD bytesReturned;

    in.PinHandle = (ULONG_PTR)hPin;
    return DeviceIoControl(hBridge, IOCTL_HDA_FREE_RT_BUFFER,
        &in, sizeof(in), NULL, 0, &bytesReturned, NULL);
}

static BOOL
BridgeSetPinState(HANDLE hBridge, HANDLE hPin, ULONG state)
{
    HDA_SET_STATE_IN in;
    DWORD bytesReturned;

    in.PinHandle = (ULONG_PTR)hPin;
    in.State = state;
    return DeviceIoControl(hBridge, IOCTL_HDA_SET_PIN_STATE,
        &in, sizeof(in), NULL, 0, &bytesReturned, NULL);
}

static volatile ULONG *
BridgeMapPosition(HANDLE hBridge, HANDLE hPin)
{
    HDA_POSITION_MAP_IN in;
    HDA_POSITION_MAP_OUT out;
    DWORD bytesReturned;

    in.PinHandle = (ULONG_PTR)hPin;
    ZeroMemory(&out, sizeof(out));

    if (!DeviceIoControl(hBridge, IOCTL_HDA_GET_RT_POSITION,
            &in, sizeof(in), &out, sizeof(out), &bytesReturned, NULL)) {
        return NULL;
    }
    return (volatile ULONG *)out.PositionRegister;
}

/* ---- Streaming thread ---- */

static DWORD WINAPI
StreamingThread(LPVOID param)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)param;
    DWORD taskIndex = 0;
    HANDLE hTask;
    ULONG lastCapPos = 0;
    ULONG halfBufferBytes;
    long bufferIndex = 0;

    /* Boost thread priority via MMCSS */
    hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    halfBufferBytes = drv->bufferSize * HDA_BLOCK_ALIGN;

    while (WaitForSingleObject(drv->hStopEvent, 0) == WAIT_TIMEOUT) {
        /*
         * Position tracking strategy:
         *   If hardware position register is available, read it directly (zero-syscall).
         *   Otherwise, use a timer-based approach.
         *
         *   The DMA buffer is split into two halves for ASIO double-buffering.
         *   When the hardware position crosses a half-buffer boundary, fire the
         *   ASIO bufferSwitch callback.
         */
        ULONG capPos = 0;

        if (drv->capturePosition) {
            capPos = *(drv->capturePosition);
        } else {
            /* Fallback: estimate position based on time */
            static LARGE_INTEGER startTime = {0};
            LARGE_INTEGER now, freq;
            if (startTime.QuadPart == 0) QueryPerformanceCounter(&startTime);
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            LONGLONG elapsed = now.QuadPart - startTime.QuadPart;
            LONGLONG bytesPlayed = (elapsed * HDA_SAMPLE_RATE * HDA_BLOCK_ALIGN) / freq.QuadPart;
            capPos = (ULONG)(bytesPlayed % drv->captureBufferSize);
        }

        /* Detect half-buffer crossing */
        ULONG lastHalf = lastCapPos / halfBufferBytes;
        ULONG curHalf = capPos / halfBufferBytes;

        if (curHalf != lastHalf) {
            /* The hardware just crossed into a new half -- process the completed half */
            bufferIndex = (curHalf == 0) ? 1 : 0;  /* Process the half that just finished */

            InterlockedExchange(&drv->currentBufferIndex, bufferIndex);

            /* Fire ASIO callback */
            if (drv->callbacks && drv->callbacks->bufferSwitch) {
                drv->callbacks->bufferSwitch(bufferIndex, ASIOTrue);
            }
        }

        lastCapPos = capPos;

        /* Sleep briefly to avoid burning CPU.
         * At 160 frames / 48kHz, each half is 3.33ms.
         * Sleep ~1ms to wake up in time. */
        Sleep(1);
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
        /* Clean up */
        if (drv->running) Asio_stop(This);
        if (drv->buffersCreated) Asio_disposeBuffers(This);

        if (drv->hCapturePin) { CloseHandle(drv->hCapturePin); drv->hCapturePin = NULL; }
        if (drv->hRenderPin) { CloseHandle(drv->hRenderPin); drv->hRenderPin = NULL; }
        if (drv->hCaptureFilter) { CloseHandle(drv->hCaptureFilter); drv->hCaptureFilter = NULL; }
        if (drv->hRenderFilter) { CloseHandle(drv->hRenderFilter); drv->hRenderFilter = NULL; }
        if (drv->hBridge) { CloseHandle(drv->hBridge); drv->hBridge = NULL; }

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

    if (drv->initialized) return ASIOTrue;

    drv->sysHandle = (HWND)sysHandle;
    drv->numInputChannels = HDA_NUM_CHANNELS;
    drv->numOutputChannels = HDA_NUM_CHANNELS;

    /* Find HDA wave filters */
    if (!FindHdaWaveFilter(&KSCATEGORY_CAPTURE, drv->captureFilterPath, 512)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "No HDA capture wave filter found");
        return ASIOFalse;
    }
    if (!FindHdaWaveFilter(&KSCATEGORY_RENDER, drv->renderFilterPath, 512)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "No HDA render wave filter found");
        return ASIOFalse;
    }

    /* Open kernel bridge driver */
    drv->hBridge = CreateFileW(
        HDA_BRIDGE_USER_PATH_W,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL
    );
    if (drv->hBridge == INVALID_HANDLE_VALUE) {
        drv->hBridge = NULL;
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot open HdaAsioBridge driver (is it installed?)");
        return ASIOFalse;
    }

    /* Open HDA wave filters */
    drv->hCaptureFilter = CreateFileW(
        drv->captureFilterPath,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL
    );
    if (drv->hCaptureFilter == INVALID_HANDLE_VALUE) {
        drv->hCaptureFilter = NULL;
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot open capture filter");
        return ASIOFalse;
    }

    drv->hRenderFilter = CreateFileW(
        drv->renderFilterPath,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL
    );
    if (drv->hRenderFilter == INVALID_HANDLE_VALUE) {
        drv->hRenderFilter = NULL;
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot open render filter");
        return ASIOFalse;
    }

    /* Create WaveRT KS pins */
    drv->hCapturePin = CreateWaveRtPin(drv->hCaptureFilter, 0, TRUE);
    if (!drv->hCapturePin) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot create capture pin (another app using exclusive mode?)");
        return ASIOFalse;
    }

    drv->hRenderPin = CreateWaveRtPin(drv->hRenderFilter, 0, FALSE);
    if (!drv->hRenderPin) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot create render pin (another app using exclusive mode?)");
        return ASIOFalse;
    }

    /* Allocate DMA buffers via kernel bridge */
    ULONG requestedSize = HDA_PREFERRED_FRAMES * HDA_BLOCK_ALIGN * 2;  /* Double buffer */

    if (!BridgeAllocBuffer(drv->hBridge, drv->hCapturePin, requestedSize,
                           &drv->captureBuffer, &drv->captureBufferSize)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot allocate capture DMA buffer (bridge error %lu)", GetLastError());
        return ASIOFalse;
    }

    if (!BridgeAllocBuffer(drv->hBridge, drv->hRenderPin, requestedSize,
                           &drv->renderBuffer, &drv->renderBufferSize)) {
        snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                 "Cannot allocate render DMA buffer (bridge error %lu)", GetLastError());
        return ASIOFalse;
    }

    /* Map position registers (optional -- may fail on some hardware) */
    drv->capturePosition = BridgeMapPosition(drv->hBridge, drv->hCapturePin);
    drv->renderPosition = BridgeMapPosition(drv->hBridge, drv->hRenderPin);

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
    return 1;
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

    if (!drv->buffersCreated) return ASE_InvalidMode;
    if (drv->running) return ASE_OK;

    /* Transition pins: STOP -> ACQUIRE -> PAUSE -> RUN */
    HANDLE pins[] = { drv->hCapturePin, drv->hRenderPin };
    ULONG states[] = { HDA_PIN_STATE_ACQUIRE, HDA_PIN_STATE_PAUSE, HDA_PIN_STATE_RUN };
    int p, s;

    for (s = 0; s < 3; s++) {
        for (p = 0; p < 2; p++) {
            if (!BridgeSetPinState(drv->hBridge, pins[p], states[s])) {
                snprintf(drv->errorMessage, sizeof(drv->errorMessage),
                         "Pin state transition to %lu failed for %s",
                         states[s], p == 0 ? "capture" : "render");
                /* Try to roll back */
                BridgeSetPinState(drv->hBridge, drv->hCapturePin, HDA_PIN_STATE_STOP);
                BridgeSetPinState(drv->hBridge, drv->hRenderPin, HDA_PIN_STATE_STOP);
                return ASE_HWMalfunction;
            }
        }
    }

    /* Start streaming thread */
    drv->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    drv->currentBufferIndex = 0;
    drv->hThread = CreateThread(NULL, 0, StreamingThread, drv, 0, NULL);
    if (!drv->hThread) {
        BridgeSetPinState(drv->hBridge, drv->hCapturePin, HDA_PIN_STATE_STOP);
        BridgeSetPinState(drv->hBridge, drv->hRenderPin, HDA_PIN_STATE_STOP);
        CloseHandle(drv->hStopEvent);
        return ASE_HWMalfunction;
    }

    drv->running = TRUE;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_stop(IASIO *This)
{
    HDA_ASIO_DRIVER *drv = (HDA_ASIO_DRIVER *)This;

    if (!drv->running) return ASE_OK;

    /* Signal thread to stop */
    SetEvent(drv->hStopEvent);
    WaitForSingleObject(drv->hThread, 5000);
    CloseHandle(drv->hThread);
    CloseHandle(drv->hStopEvent);
    drv->hThread = NULL;
    drv->hStopEvent = NULL;

    /* Transition pins back to STOP */
    BridgeSetPinState(drv->hBridge, drv->hCapturePin, HDA_PIN_STATE_PAUSE);
    BridgeSetPinState(drv->hBridge, drv->hRenderPin, HDA_PIN_STATE_PAUSE);
    BridgeSetPinState(drv->hBridge, drv->hCapturePin, HDA_PIN_STATE_ACQUIRE);
    BridgeSetPinState(drv->hBridge, drv->hRenderPin, HDA_PIN_STATE_ACQUIRE);
    BridgeSetPinState(drv->hBridge, drv->hCapturePin, HDA_PIN_STATE_STOP);
    BridgeSetPinState(drv->hBridge, drv->hRenderPin, HDA_PIN_STATE_STOP);

    drv->running = FALSE;
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
    /* Our latency is exactly one buffer period (3.33ms for 160 frames) */
    *inputLatency = drv->bufferSize > 0 ? drv->bufferSize : HDA_PREFERRED_FRAMES;
    *outputLatency = drv->bufferSize > 0 ? drv->bufferSize : HDA_PREFERRED_FRAMES;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getBufferSize(IASIO *This, long *minSize, long *maxSize,
                   long *preferredSize, long *granularity)
{
    (void)This;
    /* Hardcoded to HDA codec's aligned buffer size */
    *minSize = HDA_PREFERRED_FRAMES;
    *maxSize = HDA_PREFERRED_FRAMES * 4;    /* Up to ~13ms */
    *preferredSize = HDA_PREFERRED_FRAMES;  /* 160 frames = 3.33ms */
    *granularity = HDA_PREFERRED_FRAMES;    /* Must be multiples of 160 */
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_getSampleRate(IASIO *This, ASIOSampleRate *rate)
{
    (void)This;
    *rate = (ASIOSampleRate)HDA_SAMPLE_RATE;
    return ASE_OK;
}

static ASIOError STDMETHODCALLTYPE
Asio_setSampleRate(IASIO *This, ASIOSampleRate rate)
{
    (void)This;
    /* We only support 48kHz -- the codec's native rate */
    if ((long)rate != HDA_SAMPLE_RATE) return ASE_NoClock;
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

    if (drv->capturePosition) {
        ULONG bytePos = *(drv->capturePosition);
        *sPos = (ASIOSamples)(bytePos / HDA_BLOCK_ALIGN);
    } else {
        *sPos = 0;
    }

    QueryPerformanceCounter(&pc);
    QueryPerformanceFrequency(&freq);
    /* Convert to nanoseconds */
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

    if (!drv->initialized) return ASE_NotPresent;
    if (drv->buffersCreated) Asio_disposeBuffers(This);

    drv->bufferSize = bufferSize;
    drv->callbacks = callbacks;

    /*
     * ASIO double-buffering maps onto the DMA buffer:
     *   DMA buffer = [half 0][half 1]
     *   Each half = bufferSize frames of interleaved stereo
     *
     * For deinterleaved ASIO channels, we point directly into the
     * interleaved DMA buffer. The host/DAW handles interleaving.
     *
     * NOTE: For 16-bit stereo interleaved, the ASIO buffer pointers
     * point to the raw interleaved data. The host expects per-channel
     * buffers, so for full correctness we'd need deinterleaving.
     * For our proof-of-concept, we provide the raw interleaved buffer
     * and report 1 stereo pair per direction.
     *
     * A production driver would deinterleave in the callback thread.
     */
    ULONG halfBytes = bufferSize * HDA_BLOCK_ALIGN;

    for (i = 0; i < numChannels; i++) {
        if (bufferInfos[i].isInput) {
            long ch = bufferInfos[i].channelIndex;
            /* Point to the channel's data within each half of capture DMA buffer */
            BYTE *base = (BYTE *)drv->captureBuffer;
            /* Channel offset: ch * bytesPerSample within each frame */
            ULONG chOffset = ch * (HDA_BITS_PER_SAMPLE / 8);
            bufferInfos[i].buffers[0] = base + chOffset;
            bufferInfos[i].buffers[1] = base + halfBytes + chOffset;
        } else {
            long ch = bufferInfos[i].channelIndex;
            BYTE *base = (BYTE *)drv->renderBuffer;
            ULONG chOffset = ch * (HDA_BITS_PER_SAMPLE / 8);
            bufferInfos[i].buffers[0] = base + chOffset;
            bufferInfos[i].buffers[1] = base + halfBytes + chOffset;
        }

        /* Store in our tracking array */
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

    /* Free DMA buffers via bridge */
    if (drv->hBridge) {
        if (drv->hCapturePin) BridgeFreeBuffer(drv->hBridge, drv->hCapturePin);
        if (drv->hRenderPin) BridgeFreeBuffer(drv->hBridge, drv->hRenderPin);
    }

    drv->captureBuffer = NULL;
    drv->renderBuffer = NULL;
    drv->capturePosition = NULL;
    drv->renderPosition = NULL;
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
        L"Custom kernel-mode ASIO driver for Conexant CX20753/4\n"
        L"on Intel HDA bus.\n\n"
        L"Fixed configuration:\n"
        L"  48000 Hz / 16-bit / Stereo\n"
        L"  160 frames (3.33ms) per buffer\n\n"
        L"signal-chain experiment",
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
    /* We support outputReady -- the host should call this after filling buffers */
    return ASE_OK;
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
                   (BYTE *)"Direct HDA ASIO - signal-chain",
                   31);
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
