/*
 * asio.h -- Minimal ASIO interface definitions for C
 *
 * Implements the ASIO 2.3 interface as a COM in-process server.
 * This is a clean-room implementation based on the publicly documented
 * ASIO specification. No Steinberg SDK code is used.
 *
 * Reference: ASIO SDK documentation (Steinberg, publicly available)
 */

#ifndef ASIO_H
#define ASIO_H

#include <windows.h>
#include <unknwn.h>

/* ---- ASIO Types ---- */

typedef long ASIOBool;
#define ASIOFalse   0L
#define ASIOTrue    1L

typedef double ASIOSampleRate;

typedef long long ASIOSamples;
typedef long long ASIOTimeStamp;

/* Error codes */
typedef long ASIOError;
#define ASE_OK              0L
#define ASE_SUCCESS         0x3f4847a0L
#define ASE_NotPresent      (-1000L)
#define ASE_HWMalfunction   (-999L)
#define ASE_InvalidParameter (-998L)
#define ASE_InvalidMode     (-997L)
#define ASE_SPNotAdvancing  (-996L)
#define ASE_NoClock         (-995L)
#define ASE_NoMemory        (-994L)

/* Sample types */
typedef long ASIOSampleType;
#define ASIOSTInt16LSB      16L
#define ASIOSTInt24LSB      17L
#define ASIOSTInt32LSB      18L
#define ASIOSTFloat32LSB    19L
#define ASIOSTFloat64LSB    20L
#define ASIOSTInt16MSB      0L
#define ASIOSTInt24MSB      1L
#define ASIOSTInt32MSB      2L
#define ASIOSTFloat32MSB    3L
#define ASIOSTFloat64MSB    4L

/* ---- ASIO Structures ---- */

typedef struct ASIODriverInfo {
    long    asioVersion;        /* currently 2 */
    long    driverVersion;      /* driver-specific */
    char    name[32];           /* driver name */
    char    errorMessage[124];  /* last error message */
    void   *sysRef;             /* system reference (HWND on Windows) */
} ASIODriverInfo;

typedef struct ASIOClockSource {
    long    index;
    long    associatedChannel;
    long    associatedGroup;
    ASIOBool isCurrentSource;
    char    name[32];
} ASIOClockSource;

typedef struct ASIOChannelInfo {
    long            channel;
    ASIOBool        isInput;
    ASIOBool        isActive;
    long            channelGroup;
    ASIOSampleType  type;
    char            name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo {
    ASIOBool    isInput;
    long        channelIndex;
    void       *buffers[2];     /* double buffer: [0] and [1] */
} ASIOBufferInfo;

/* Time info (simplified -- full ASIO 2.3 has more fields) */
typedef struct ASIOTimeCode {
    double      speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char        future[64];
} ASIOTimeCode;

typedef struct AsioTimeInfo {
    double      speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char        reserved[12];
} AsioTimeInfo;

typedef struct ASIOTime {
    long        reserved[4];
    AsioTimeInfo timeInfo;
    ASIOTimeCode timeCode;
} ASIOTime;

/* ASIO message selectors */
#define kAsioSelectorSupported      1L
#define kAsioEngineVersion          2L
#define kAsioResetRequest           3L
#define kAsioBufferSizeChange       4L
#define kAsioResyncRequest          5L
#define kAsioLatenciesChanged       6L
#define kAsioSupportsTimeInfo       7L
#define kAsioSupportsTimeCode       8L
#define kAsioSupportsInputMonitor   12L

/* ---- ASIO Callbacks (provided by host/DAW) ---- */

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void *message, double *opt);
    ASIOTime *(*bufferSwitchTimeInfo)(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

/* ---- IASIO COM Interface ---- */

/*
 * The ASIO driver is a COM object. The vtable matches the C++ abstract class
 * layout: IUnknown methods first, then ASIO-specific methods.
 *
 * In C, each method takes an explicit 'this' pointer as first parameter.
 */

/* Forward declaration */
typedef struct IASIO IASIO;

typedef struct IASIOVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IASIO *This, REFIID riid, void **ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IASIO *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IASIO *This);

    /* IASIO */
    ASIOBool  (STDMETHODCALLTYPE *init)(IASIO *This, void *sysHandle);
    void      (STDMETHODCALLTYPE *getDriverName)(IASIO *This, char *name);
    long      (STDMETHODCALLTYPE *getDriverVersion)(IASIO *This);
    void      (STDMETHODCALLTYPE *getErrorMessage)(IASIO *This, char *string);
    ASIOError (STDMETHODCALLTYPE *start)(IASIO *This);
    ASIOError (STDMETHODCALLTYPE *stop)(IASIO *This);
    ASIOError (STDMETHODCALLTYPE *getChannels)(IASIO *This, long *numInputChannels, long *numOutputChannels);
    ASIOError (STDMETHODCALLTYPE *getLatencies)(IASIO *This, long *inputLatency, long *outputLatency);
    ASIOError (STDMETHODCALLTYPE *getBufferSize)(IASIO *This, long *minSize, long *maxSize, long *preferredSize, long *granularity);
    ASIOError (STDMETHODCALLTYPE *canSampleRate)(IASIO *This, ASIOSampleRate sampleRate);
    ASIOError (STDMETHODCALLTYPE *getSampleRate)(IASIO *This, ASIOSampleRate *sampleRate);
    ASIOError (STDMETHODCALLTYPE *setSampleRate)(IASIO *This, ASIOSampleRate sampleRate);
    ASIOError (STDMETHODCALLTYPE *getClockSources)(IASIO *This, ASIOClockSource *clocks, long *numSources);
    ASIOError (STDMETHODCALLTYPE *setClockSource)(IASIO *This, long reference);
    ASIOError (STDMETHODCALLTYPE *getSamplePosition)(IASIO *This, ASIOSamples *sPos, ASIOTimeStamp *tStamp);
    ASIOError (STDMETHODCALLTYPE *getChannelInfo)(IASIO *This, ASIOChannelInfo *info);
    ASIOError (STDMETHODCALLTYPE *createBuffers)(IASIO *This, ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks);
    ASIOError (STDMETHODCALLTYPE *disposeBuffers)(IASIO *This);
    ASIOError (STDMETHODCALLTYPE *controlPanel)(IASIO *This);
    ASIOError (STDMETHODCALLTYPE *future)(IASIO *This, long selector, void *opt);
    ASIOError (STDMETHODCALLTYPE *outputReady)(IASIO *This);
} IASIOVtbl;

struct IASIO {
    IASIOVtbl *lpVtbl;
};

/* ---- Our driver's CLSID ---- */

/* {E7B5C4A1-3F82-4D67-9A15-8C6D2E1F0B34} */
DEFINE_GUID(CLSID_HdaDirectAsio,
    0xe7b5c4a1, 0x3f82, 0x4d67,
    0x9a, 0x15, 0x8c, 0x6d, 0x2e, 0x1f, 0x0b, 0x34);

#define HDA_ASIO_DRIVER_NAME    "HDA Direct ASIO"
#define HDA_ASIO_CLSID_STR     "{E7B5C4A1-3F82-4D67-9A15-8C6D2E1F0B34}"

#endif /* ASIO_H */
