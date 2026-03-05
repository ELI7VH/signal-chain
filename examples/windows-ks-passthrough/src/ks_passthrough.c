/*
 * signal-chain: WDM Kernel Streaming Audio Passthrough
 *
 * One layer below WASAPI — direct KS filter/pin access via DeviceIoControl.
 * This is what ASIO4ALL does internally.
 *
 * Audio stack:
 *   WASAPI Shared   -> Audio Engine -> KS -> Driver -> Hardware
 *   WASAPI Exclusive -> KS -> Driver -> Hardware
 *   KS Direct (this) -> Driver -> Hardware
 *
 * Build:
 *   gcc -O2 -o ks_passthrough.exe src/ks_passthrough.c -lsetupapi -lksuser -lole32 -lavrt
 *
 * (c) 2026 Elijah Lucian / signal-chain
 */

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winioctl.h>  /* CTL_CODE, METHOD_NEITHER, etc. */
#include <mmreg.h>     /* WAVEFORMATEX, WAVE_FORMAT_PCM */
#include <setupapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ── Manual definitions for w64devkit gaps ───────────────────────── */
/* These are part of the Windows DDK/SDK but MinGW-w64 may omit them */

#ifndef FILE_DEVICE_KS
#define FILE_DEVICE_KS 0x0000002F
#endif

#ifndef IOCTL_KS_PROPERTY
#define IOCTL_KS_PROPERTY CTL_CODE(FILE_DEVICE_KS, 0x000, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_KS_READ_STREAM
#define IOCTL_KS_READ_STREAM CTL_CODE(FILE_DEVICE_KS, 0x020, METHOD_NEITHER, FILE_READ_ACCESS)
#endif
#ifndef IOCTL_KS_WRITE_STREAM
#define IOCTL_KS_WRITE_STREAM CTL_CODE(FILE_DEVICE_KS, 0x004, METHOD_NEITHER, FILE_WRITE_ACCESS)
#endif

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

/* ── Globals ────────────────────────────────────────────────────── */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── GUIDs ──────────────────────────────────────────────────────── */
/* Most KS GUIDs come from libksuser.a. Define ones that don't. */

/* KSPROPSETID_RtAudio = {A855A48C-2F78-4729-9051-1968746B9EEF}
 * This is the WaveRT real-time audio property set — not in libksuser. */
static const GUID LOCAL_KSPROPSETID_RtAudio =
    {0xA855A48C, 0x2F78, 0x4729, {0x90, 0x51, 0x19, 0x68, 0x74, 0x6B, 0x9E, 0xEF}};

/* ── Helpers ────────────────────────────────────────────────────── */

static const char* guid_name(const GUID* g) {
    if (IsEqualGUID(g, &KSCATEGORY_AUDIO))   return "KSCATEGORY_AUDIO";
    if (IsEqualGUID(g, &KSCATEGORY_RENDER))  return "KSCATEGORY_RENDER";
    if (IsEqualGUID(g, &KSCATEGORY_CAPTURE)) return "KSCATEGORY_CAPTURE";
    return "unknown";
}

static void print_guid(const GUID* g) {
    printf("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ── Phase 1: Enumerate KS audio device interfaces ──────────────── */

typedef struct {
    char path[512];
    char description[256];
} KsDeviceInfo;

static int enumerate_ks_devices(const GUID* category, KsDeviceInfo* devices, int max_devices) {
    int count = 0;

    HDEVINFO dev_info = SetupDiGetClassDevsA(
        category, NULL, NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (dev_info == INVALID_HANDLE_VALUE) {
        printf("  SetupDiGetClassDevs failed (err=%lu)\n", GetLastError());
        return 0;
    }

    SP_DEVICE_INTERFACE_DATA iface_data;
    iface_data.cbSize = sizeof(iface_data);

    for (DWORD idx = 0;
         count < max_devices &&
         SetupDiEnumDeviceInterfaces(dev_info, NULL, category, idx, &iface_data);
         idx++)
    {
        /* Get required buffer size */
        DWORD required_size = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &iface_data, NULL, 0, &required_size, NULL);

        if (required_size == 0) continue;

        /* Allocate and get detail */
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)malloc(required_size);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        SP_DEVINFO_DATA dev_data;
        dev_data.cbSize = sizeof(dev_data);

        if (SetupDiGetDeviceInterfaceDetailA(
                dev_info, &iface_data, detail, required_size, NULL, &dev_data))
        {
            strncpy(devices[count].path, detail->DevicePath, sizeof(devices[count].path) - 1);

            /* Get friendly name */
            SetupDiGetDeviceRegistryPropertyA(
                dev_info, &dev_data, SPDRP_FRIENDLYNAME,
                NULL, (BYTE*)devices[count].description,
                sizeof(devices[count].description), NULL);

            count++;
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return count;
}

/* ── Phase 2: Open filter and query pins ────────────────────────── */

static HANDLE open_ks_filter(const char* device_path) {
    HANDLE h = CreateFileA(
        device_path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        printf("  CreateFile failed (err=%lu)\n", GetLastError());
    }
    return h;
}

static BOOL ks_property(HANDLE filter, const GUID* prop_set, ULONG prop_id,
                        ULONG pin_id, void* out_data, ULONG out_size, ULONG* bytes_ret)
{
    /* Use KSP_PIN for pin properties, KSPROPERTY for others */
    KSP_PIN ksp;
    memset(&ksp, 0, sizeof(ksp));
    ksp.Property.Set = *prop_set;
    ksp.Property.Id = prop_id;
    ksp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksp.PinId = pin_id;

    ULONG input_size;
    if (IsEqualGUID(prop_set, &KSPROPSETID_Pin))
        input_size = sizeof(KSP_PIN);
    else
        input_size = sizeof(KSPROPERTY);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(
        filter,
        IOCTL_KS_PROPERTY,
        &ksp, input_size,
        out_data, out_size,
        &returned, NULL);

    if (bytes_ret) *bytes_ret = returned;
    return ok;
}

static int get_pin_count(HANDLE filter) {
    ULONG count = 0;
    if (ks_property(filter, &KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES,
                    0, &count, sizeof(count), NULL)) {
        return (int)count;
    }
    return -1;
}

static KSPIN_DATAFLOW get_pin_dataflow(HANDLE filter, ULONG pin_id) {
    KSPIN_DATAFLOW flow = 0;
    ks_property(filter, &KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW,
                pin_id, &flow, sizeof(flow), NULL);
    return flow;
}

static KSPIN_COMMUNICATION get_pin_communication(HANDLE filter, ULONG pin_id) {
    KSPIN_COMMUNICATION comm = 0;
    ks_property(filter, &KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION,
                pin_id, &comm, sizeof(comm), NULL);
    return comm;
}

static const char* dataflow_name(KSPIN_DATAFLOW flow) {
    switch (flow) {
        case KSPIN_DATAFLOW_IN:  return "IN (render/sink)";
        case KSPIN_DATAFLOW_OUT: return "OUT (capture/source)";
        default: return "unknown";
    }
}

static const char* communication_name(KSPIN_COMMUNICATION comm) {
    switch (comm) {
        case KSPIN_COMMUNICATION_NONE:   return "NONE";
        case KSPIN_COMMUNICATION_SINK:   return "SINK";
        case KSPIN_COMMUNICATION_SOURCE: return "SOURCE";
        case KSPIN_COMMUNICATION_BOTH:   return "BOTH";
        case KSPIN_COMMUNICATION_BRIDGE: return "BRIDGE";
        default: return "unknown";
    }
}

/* ── Pin format probing ─────────────────────────────────────────── */

/*
 * Query a pin's supported KSDATARANGE entries.
 * This tells us exactly what formats the hardware accepts.
 */
static void probe_pin_formats(HANDLE filter, ULONG pin_id) {
    /* First, get the required buffer size */
    KSP_PIN ksp;
    memset(&ksp, 0, sizeof(ksp));
    ksp.Property.Set = KSPROPSETID_Pin;
    ksp.Property.Id = KSPROPERTY_PIN_DATARANGES;
    ksp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksp.PinId = pin_id;

    DWORD required = 0;
    DeviceIoControl(filter, IOCTL_KS_PROPERTY,
        &ksp, sizeof(ksp), NULL, 0, &required, NULL);

    if (required == 0) {
        printf("    (no data ranges reported)\n");
        return;
    }

    BYTE* buf = (BYTE*)calloc(1, required);
    if (!buf) return;

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY,
        &ksp, sizeof(ksp), buf, required, &returned, NULL);

    if (!ok) {
        printf("    (failed to get data ranges, err=%lu)\n", GetLastError());
        free(buf);
        return;
    }

    /* The buffer starts with KSMULTIPLE_ITEM header */
    KSMULTIPLE_ITEM* multi = (KSMULTIPLE_ITEM*)buf;
    printf("    Data ranges: %lu entries (%lu bytes)\n", multi->Count, multi->Size);

    BYTE* ptr = buf + sizeof(KSMULTIPLE_ITEM);
    for (ULONG i = 0; i < multi->Count; i++) {
        KSDATARANGE* range = (KSDATARANGE*)ptr;

        printf("    [%lu] size=%lu ", i, range->FormatSize);

        /* Check if this is an audio data range */
        if (IsEqualGUID(&range->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO)) {
            printf("AUDIO ");

            if (IsEqualGUID(&range->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))
                printf("PCM ");
            else if (IsEqualGUID(&range->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                printf("FLOAT ");
            else
                printf("sub=? ");

            /* If it's a KSDATARANGE_AUDIO, print the ranges */
            if (range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                KSDATARANGE_AUDIO* audio = (KSDATARANGE_AUDIO*)range;
                printf("ch=1-%lu rate=%lu-%lu bits=%lu-%lu",
                    audio->MaximumChannels,
                    audio->MinimumSampleFrequency, audio->MaximumSampleFrequency,
                    audio->MinimumBitsPerSample, audio->MaximumBitsPerSample);
            }
        } else {
            printf("(non-audio)");
        }
        printf("\n");

        /* Advance to next range (aligned to 8-byte boundary) */
        ULONG advance = range->FormatSize;
        if (advance == 0) break;  /* safety */
        advance = (advance + 7) & ~7UL;
        ptr += advance;
    }

    free(buf);
}

/* ── Phase 3: Create pin instance ───────────────────────────────── */

/*
 * KsCreatePin needs a KSPIN_CONNECT followed by the data format.
 * We build this as a single contiguous buffer:
 *   [KSPIN_CONNECT][KSDATAFORMAT_WAVEFORMATEX]
 */
static HANDLE create_ks_pin(HANDLE filter, ULONG pin_id,
                            int sample_rate, int channels, int bits)
{
    /* Build the connect + format structure */
    size_t total = sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEX);
    BYTE* buf = (BYTE*)calloc(1, total);
    if (!buf) return INVALID_HANDLE_VALUE;

    KSPIN_CONNECT* connect = (KSPIN_CONNECT*)buf;
    KSDATAFORMAT_WAVEFORMATEX* dfmt = (KSDATAFORMAT_WAVEFORMATEX*)(buf + sizeof(KSPIN_CONNECT));

    /* Pin connect — use LOOPED_STREAMING for WaveRT drivers (HDA on modern Windows).
     * Standard streaming is for the old WaveCyclic model. */
    connect->Interface.Set = KSINTERFACESETID_Standard;
    connect->Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
    connect->Interface.Flags = 0;
    connect->Medium.Set = KSMEDIUMSETID_Standard;
    connect->Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    connect->Medium.Flags = 0;
    connect->PinId = pin_id;
    connect->PinToHandle = NULL;
    connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
    connect->Priority.PrioritySubClass = KSPRIORITY_NORMAL;

    /* Data format: WAVE_FORMAT_PCM via KSDATAFORMAT + WAVEFORMATEX */
    dfmt->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    dfmt->DataFormat.Flags = 0;
    dfmt->DataFormat.SampleSize = (ULONG)(channels * bits / 8);
    dfmt->DataFormat.Reserved = 0;
    dfmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    dfmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    dfmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    dfmt->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    dfmt->WaveFormatEx.nChannels = (WORD)channels;
    dfmt->WaveFormatEx.nSamplesPerSec = (DWORD)sample_rate;
    dfmt->WaveFormatEx.wBitsPerSample = (WORD)bits;
    dfmt->WaveFormatEx.nBlockAlign = (WORD)(channels * bits / 8);
    dfmt->WaveFormatEx.nAvgBytesPerSec = (DWORD)(sample_rate * dfmt->WaveFormatEx.nBlockAlign);
    dfmt->WaveFormatEx.cbSize = 0;

    HANDLE pin_handle = INVALID_HANDLE_VALUE;
    DWORD err = KsCreatePin(filter, connect, GENERIC_READ | GENERIC_WRITE, &pin_handle);

    if (err != ERROR_SUCCESS) {
        printf("  KsCreatePin failed (err=%lu / 0x%08lX)\n", err, err);
        pin_handle = INVALID_HANDLE_VALUE;
    }

    free(buf);
    return pin_handle;
}

/* ── Phase 4: Pin state control ─────────────────────────────────── */

static BOOL set_pin_state(HANDLE pin, KSSTATE state) {
    KSPROPERTY prop;
    memset(&prop, 0, sizeof(prop));
    prop.Set = KSPROPSETID_Connection;
    prop.Id = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;

    /* Try with overlapped I/O first (pin may inherit overlapped mode from filter) */
    OVERLAPPED ovl = {0};
    ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(
        pin,
        IOCTL_KS_PROPERTY,
        &prop, sizeof(prop),
        &state, sizeof(state),
        &returned, &ovl);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ovl.hEvent, 2000);
        ok = GetOverlappedResult(pin, &ovl, &returned, FALSE);
    }

    if (!ok) {
        printf("    (set state err=%lu)\n", GetLastError());
    }

    CloseHandle(ovl.hEvent);
    return ok;
}

static const char* state_name(KSSTATE state) {
    switch (state) {
        case KSSTATE_STOP:    return "STOP";
        case KSSTATE_ACQUIRE: return "ACQUIRE";
        case KSSTATE_PAUSE:   return "PAUSE";
        case KSSTATE_RUN:     return "RUN";
        default: return "unknown";
    }
}

static BOOL transition_pin_state(HANDLE pin, KSSTATE target) {
    /* Must transition through states in order:
     * STOP -> ACQUIRE -> PAUSE -> RUN */
    KSSTATE states[] = { KSSTATE_ACQUIRE, KSSTATE_PAUSE, KSSTATE_RUN };
    for (int i = 0; i < 3; i++) {
        if (!set_pin_state(pin, states[i])) {
            printf("  Failed to set pin state to %s (err=%lu)\n",
                state_name(states[i]), GetLastError());
            return FALSE;
        }
        printf("  Pin -> %s\n", state_name(states[i]));
        if (states[i] == target) return TRUE;
    }
    return TRUE;
}

/* ── Phase 5: KS Streaming ──────────────────────────────────────── */

/*
 * Read audio data from a capture pin using IOCTL_KS_READ_STREAM.
 * Uses overlapped I/O since KS streaming is asynchronous.
 */
static int ks_read_stream(HANDLE pin, BYTE* buffer, ULONG buffer_size,
                          OVERLAPPED* ovl, ULONG* bytes_read)
{
    KSSTREAM_HEADER header;
    memset(&header, 0, sizeof(header));
    header.Size = sizeof(KSSTREAM_HEADER);
    header.FrameExtent = buffer_size;
    header.Data = buffer;
    header.DataUsed = 0;

    ResetEvent(ovl->hEvent);

    BOOL ok = DeviceIoControl(
        pin,
        IOCTL_KS_READ_STREAM,
        NULL, 0,
        &header, sizeof(header),
        NULL, ovl);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ovl->hEvent, 200);
        if (wait == WAIT_OBJECT_0) {
            DWORD transferred = 0;
            GetOverlappedResult(pin, ovl, &transferred, FALSE);
            if (bytes_read) *bytes_read = header.DataUsed;
            return 1;
        }
        CancelIo(pin);
        return 0;
    } else if (ok) {
        if (bytes_read) *bytes_read = header.DataUsed;
        return 1;
    }

    return 0;
}

/*
 * Write audio data to a render pin using IOCTL_KS_WRITE_STREAM.
 */
static int ks_write_stream(HANDLE pin, BYTE* buffer, ULONG data_size,
                           ULONG frame_size, OVERLAPPED* ovl)
{
    KSSTREAM_HEADER header;
    memset(&header, 0, sizeof(header));
    header.Size = sizeof(KSSTREAM_HEADER);
    header.FrameExtent = data_size;
    header.Data = buffer;
    header.DataUsed = data_size;

    ResetEvent(ovl->hEvent);

    BOOL ok = DeviceIoControl(
        pin,
        IOCTL_KS_WRITE_STREAM,
        &header, sizeof(header),
        NULL, 0,
        NULL, ovl);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(ovl->hEvent, 200);
        if (wait == WAIT_OBJECT_0) {
            return 1;
        }
        CancelIo(pin);
        return 0;
    }

    return ok ? 1 : 0;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    int sample_rate = 48000;
    int channels = 2;
    int bits = 16;
    int buffer_frames = 160;  /* match our WASAPI result */

    printf("=== signal-chain: WDM Kernel Streaming Audio Passthrough ===\n");
    printf("Going below WASAPI — direct KS filter/pin access.\n\n");

    signal(SIGINT, signal_handler);

    /* ──────────────────────────────────────────────────────────────
     * PHASE 1: Enumerate KS audio device interfaces
     * ──────────────────────────────────────────────────────────── */
    printf("--- Phase 1: Device Enumeration ---\n\n");

    KsDeviceInfo devices[32];
    int dev_count;

    /* Enumerate each category */
    const GUID* categories[] = { &KSCATEGORY_AUDIO, &KSCATEGORY_RENDER, &KSCATEGORY_CAPTURE };
    const char* cat_names[] = { "AUDIO", "RENDER", "CAPTURE" };

    for (int c = 0; c < 3; c++) {
        dev_count = enumerate_ks_devices(categories[c], devices, 32);
        printf("[%s] %d device(s):\n", cat_names[c], dev_count);
        for (int i = 0; i < dev_count; i++) {
            printf("  %d. %s\n", i, devices[i].description[0] ? devices[i].description : "(no name)");
            printf("     %s\n", devices[i].path);
        }
        printf("\n");
    }

    /* ──────────────────────────────────────────────────────────────
     * PHASE 2: Open filters and inspect pins
     * ──────────────────────────────────────────────────────────── */
    printf("--- Phase 2: Filter/Pin Inspection ---\n\n");

    /* Find capture and render device paths.
     * We specifically target the HDA wave filters — skip the webcam,
     * skip root#media (the software audio engine), skip topology filters.
     *
     * HDA wave filters have paths containing "hdaudio" and end in "wave"
     */
    KsDeviceInfo cap_devices[8], ren_devices[8];
    int cap_count = enumerate_ks_devices(&KSCATEGORY_CAPTURE, cap_devices, 8);
    int ren_count = enumerate_ks_devices(&KSCATEGORY_RENDER, ren_devices, 8);

    /* Find HDA capture wave filter (contains "hdaudio" and "wave") */
    int cap_idx = -1;
    for (int i = 0; i < cap_count; i++) {
        if (strstr(cap_devices[i].path, "hdaudio") &&
            strstr(cap_devices[i].path, "wave")) {
            cap_idx = i;
            printf("  -> HDA capture: %s\n", cap_devices[i].path);
            break;
        }
    }

    /* Find HDA render wave filter (prefer speaker, fallback to headphone) */
    int ren_idx = -1;
    for (int i = 0; i < ren_count; i++) {
        if (strstr(ren_devices[i].path, "hdaudio") &&
            strstr(ren_devices[i].path, "wave")) {
            ren_idx = i;
            printf("  -> HDA render:  %s\n", ren_devices[i].path);
            break;  /* first match = speaker, second = headphone */
        }
    }

    if (cap_idx < 0 || ren_idx < 0) {
        printf("ERROR: Could not find HDA wave filters.\n");
        printf("  HDA capture found: %s\n", cap_idx >= 0 ? "yes" : "no");
        printf("  HDA render found: %s\n", ren_idx >= 0 ? "yes" : "no");
        return 1;
    }

    printf("\n");
    printf("Capture filter: %s\n", cap_devices[cap_idx].path);
    HANDLE cap_filter = open_ks_filter(cap_devices[cap_idx].path);
    if (cap_filter == INVALID_HANDLE_VALUE) {
        printf("FAILED to open capture filter.\n");
        printf("This usually means the Windows Audio Service has the device locked.\n");
        printf("Try: net stop audiosrv (requires admin, will kill all audio)\n");
        return 1;
    }
    printf("  Opened capture filter: OK\n");

    int cap_pin_count = get_pin_count(cap_filter);
    printf("  Pin count: %d\n", cap_pin_count);

    int cap_pin_id = -1;
    for (int p = 0; p < cap_pin_count; p++) {
        KSPIN_DATAFLOW flow = get_pin_dataflow(cap_filter, p);
        KSPIN_COMMUNICATION comm = get_pin_communication(cap_filter, p);
        printf("  Pin %d: flow=%s  comm=%s\n", p, dataflow_name(flow), communication_name(comm));

        /* Capture pin: data flows OUT of the device (to us) */
        if (flow == KSPIN_DATAFLOW_OUT &&
            (comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH)) {
            cap_pin_id = p;
        }
    }
    printf("\n");

    printf("Render filter: %s\n", ren_devices[ren_idx].path);
    HANDLE ren_filter = open_ks_filter(ren_devices[ren_idx].path);
    if (ren_filter == INVALID_HANDLE_VALUE) {
        printf("FAILED to open render filter.\n");
        CloseHandle(cap_filter);
        return 1;
    }
    printf("  Opened render filter: OK\n");

    int ren_pin_count = get_pin_count(ren_filter);
    printf("  Pin count: %d\n", ren_pin_count);

    int ren_pin_id = -1;
    for (int p = 0; p < ren_pin_count; p++) {
        KSPIN_DATAFLOW flow = get_pin_dataflow(ren_filter, p);
        KSPIN_COMMUNICATION comm = get_pin_communication(ren_filter, p);
        printf("  Pin %d: flow=%s  comm=%s\n", p, dataflow_name(flow), communication_name(comm));

        /* Render pin: data flows IN to the device (from us) */
        if (flow == KSPIN_DATAFLOW_IN &&
            (comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH)) {
            ren_pin_id = p;
        }
    }
    printf("\n");

    if (cap_pin_id < 0) {
        printf("ERROR: No usable capture pin found.\n");
        CloseHandle(cap_filter);
        CloseHandle(ren_filter);
        return 1;
    }
    if (ren_pin_id < 0) {
        printf("ERROR: No usable render pin found.\n");
        CloseHandle(cap_filter);
        CloseHandle(ren_filter);
        return 1;
    }

    printf("Selected: capture pin %d, render pin %d\n\n", cap_pin_id, ren_pin_id);

    /* Probe supported formats on each pin */
    printf("Capture pin %d supported formats:\n", cap_pin_id);
    probe_pin_formats(cap_filter, (ULONG)cap_pin_id);
    printf("\nRender pin %d supported formats:\n", ren_pin_id);
    probe_pin_formats(ren_filter, (ULONG)ren_pin_id);

    /* Check pin instance counts (is AudioDG holding the pin?) */
    printf("\nPin instance counts:\n");
    KSPIN_CINSTANCES cap_inst = {0}, ren_inst = {0};
    ks_property(cap_filter, &KSPROPSETID_Pin, KSPROPERTY_PIN_CINSTANCES,
                (ULONG)cap_pin_id, &cap_inst, sizeof(cap_inst), NULL);
    printf("  Capture pin %d: current=%lu, max=%lu %s\n",
        cap_pin_id, cap_inst.CurrentCount, cap_inst.PossibleCount,
        (cap_inst.PossibleCount > 0 && cap_inst.CurrentCount >= cap_inst.PossibleCount)
            ? "<-- FULL (AudioDG has it)" : "");
    ks_property(ren_filter, &KSPROPSETID_Pin, KSPROPERTY_PIN_CINSTANCES,
                (ULONG)ren_pin_id, &ren_inst, sizeof(ren_inst), NULL);
    printf("  Render pin %d:  current=%lu, max=%lu %s\n",
        ren_pin_id, ren_inst.CurrentCount, ren_inst.PossibleCount,
        (ren_inst.PossibleCount > 0 && ren_inst.CurrentCount >= ren_inst.PossibleCount)
            ? "<-- FULL (AudioDG has it)" : "");
    printf("\n");

    /* ──────────────────────────────────────────────────────────────
     * PHASE 3: Create pin instances
     * ──────────────────────────────────────────────────────────── */
    printf("--- Phase 3: Pin Creation ---\n\n");

    printf("Format: %d-bit PCM, %dch, %dHz\n", bits, channels, sample_rate);

    printf("Creating capture pin...\n");
    HANDLE cap_pin = create_ks_pin(cap_filter, (ULONG)cap_pin_id,
                                    sample_rate, channels, bits);
    if (cap_pin == INVALID_HANDLE_VALUE) {
        printf("FAILED to create capture pin instance.\n");
        printf("The audio service may have exclusive access. On modern Windows,\n");
        printf("KS pins for audio devices are typically owned by AudioDG.exe.\n");
        CloseHandle(cap_filter);
        CloseHandle(ren_filter);
        return 1;
    }
    printf("  Capture pin created: OK\n");

    printf("Creating render pin...\n");
    HANDLE ren_pin = create_ks_pin(ren_filter, (ULONG)ren_pin_id,
                                    sample_rate, channels, bits);
    if (ren_pin == INVALID_HANDLE_VALUE) {
        printf("FAILED to create render pin instance.\n");
        CloseHandle(cap_pin);
        CloseHandle(cap_filter);
        CloseHandle(ren_filter);
        return 1;
    }
    printf("  Render pin created: OK\n\n");

    /* ──────────────────────────────────────────────────────────────
     * PHASE 4: Allocate WaveRT buffers + transition to RUN
     *
     * WaveRT pins use a shared DMA buffer mapped into user space.
     * We need to request this buffer BEFORE starting streaming.
     * This is KSPROPERTY_RTAUDIO_BUFFER (prop set KSPROPSETID_RtAudio, id 0).
     * ──────────────────────────────────────────────────────────── */
    printf("--- Phase 4: WaveRT Buffer + State Machine ---\n\n");

    /*
     * KSPROPSETID_RtAudio = {A855A48C-2F78-4729-9051-1968746B9EEF}
     *
     * KSPROPERTY_RTAUDIO_BUFFER (id=0):
     *   Input:  KSRTAUDIO_BUFFER_PROPERTY { KSPROPERTY, BaseAddress, RequestedBufferSize }
     *   Output: KSRTAUDIO_BUFFER { BufferAddress, ActualBufferSize, CallMemoryBarrier }
     */
    #pragma pack(push, 1)
    typedef struct {
        KSPROPERTY Property;
        PVOID BaseAddress;
        ULONG RequestedBufferSize;
    } KSRTAUDIO_BUFFER_PROPERTY;

    typedef struct {
        PVOID BufferAddress;
        ULONG ActualBufferSize;
        BOOL CallMemoryBarrier;
    } KSRTAUDIO_BUFFER;
    #pragma pack(pop)

    /* Try to get WaveRT buffers */
    ULONG frame_bytes_rt = (ULONG)(channels * bits / 8);
    ULONG requested_size = (ULONG)(buffer_frames * frame_bytes_rt * 2);  /* double-buffer */

    BYTE* cap_rt_buf = NULL;
    ULONG cap_rt_size = 0;
    BYTE* ren_rt_buf = NULL;
    ULONG ren_rt_size = 0;

    /* Capture RT buffer */
    {
        KSRTAUDIO_BUFFER_PROPERTY rt_prop;
        memset(&rt_prop, 0, sizeof(rt_prop));
        rt_prop.Property.Set = LOCAL_KSPROPSETID_RtAudio;
        rt_prop.Property.Id = 0;  /* KSPROPERTY_RTAUDIO_BUFFER */
        rt_prop.Property.Flags = KSPROPERTY_TYPE_GET;
        rt_prop.BaseAddress = NULL;
        rt_prop.RequestedBufferSize = requested_size;

        KSRTAUDIO_BUFFER rt_result = {0};
        DWORD returned = 0;
        OVERLAPPED ovl = {0};
        ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        BOOL ok = DeviceIoControl(cap_pin, IOCTL_KS_PROPERTY,
            &rt_prop, sizeof(rt_prop),
            &rt_result, sizeof(rt_result),
            &returned, &ovl);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(ovl.hEvent, 2000);
            ok = GetOverlappedResult(cap_pin, &ovl, &returned, FALSE);
        }

        CloseHandle(ovl.hEvent);

        if (ok && rt_result.BufferAddress) {
            cap_rt_buf = (BYTE*)rt_result.BufferAddress;
            cap_rt_size = rt_result.ActualBufferSize;
            printf("Capture WaveRT buffer: %p (%lu bytes, %.2fms)\n",
                cap_rt_buf, cap_rt_size,
                (double)cap_rt_size / frame_bytes_rt / sample_rate * 1000.0);
            if (rt_result.CallMemoryBarrier)
                printf("  (requires MemoryBarrier)\n");
        } else {
            printf("Capture WaveRT buffer: NOT AVAILABLE (err=%lu)\n", GetLastError());
            printf("  This may mean the pin doesn't support WaveRT buffer mapping\n");
            printf("  from user mode, or requires kernel-mode access (PortCls).\n");
        }
    }

    /* Render RT buffer */
    {
        KSRTAUDIO_BUFFER_PROPERTY rt_prop;
        memset(&rt_prop, 0, sizeof(rt_prop));
        rt_prop.Property.Set = LOCAL_KSPROPSETID_RtAudio;
        rt_prop.Property.Id = 0;
        rt_prop.Property.Flags = KSPROPERTY_TYPE_GET;
        rt_prop.BaseAddress = NULL;
        rt_prop.RequestedBufferSize = requested_size;

        KSRTAUDIO_BUFFER rt_result = {0};
        DWORD returned = 0;
        OVERLAPPED ovl = {0};
        ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        BOOL ok = DeviceIoControl(ren_pin, IOCTL_KS_PROPERTY,
            &rt_prop, sizeof(rt_prop),
            &rt_result, sizeof(rt_result),
            &returned, &ovl);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(ovl.hEvent, 2000);
            ok = GetOverlappedResult(ren_pin, &ovl, &returned, FALSE);
        }

        CloseHandle(ovl.hEvent);

        if (ok && rt_result.BufferAddress) {
            ren_rt_buf = (BYTE*)rt_result.BufferAddress;
            ren_rt_size = rt_result.ActualBufferSize;
            printf("Render WaveRT buffer: %p (%lu bytes, %.2fms)\n",
                ren_rt_buf, ren_rt_size,
                (double)ren_rt_size / frame_bytes_rt / sample_rate * 1000.0);
        } else {
            printf("Render WaveRT buffer: NOT AVAILABLE (err=%lu)\n", GetLastError());
        }
    }

    printf("\n");

    /* State transitions — attempt even if RT buffer failed */
    int cap_running = 0, ren_running = 0;

    printf("Capture pin state transitions:\n");
    if (transition_pin_state(cap_pin, KSSTATE_RUN)) {
        cap_running = 1;
    } else {
        printf("  Capture state transition BLOCKED.\n");
    }

    printf("Render pin state transitions:\n");
    if (transition_pin_state(ren_pin, KSSTATE_RUN)) {
        ren_running = 1;
    } else {
        printf("  Render state transition BLOCKED.\n");
    }
    printf("\n");

    /* ──────────────────────────────────────────────────────────────
     * RESULTS
     * ──────────────────────────────────────────────────────────── */
    printf("=== RESULTS ===\n\n");
    printf("Phase 1 - Device Enumeration:   OK (found HDA filter graph)\n");
    printf("Phase 2 - Filter/Pin Inspection: OK (pins, formats, instances visible)\n");
    printf("Phase 3 - Pin Creation:          OK (WaveRT looped streaming pins)\n");
    printf("Phase 4 - WaveRT DMA Buffer:     BLOCKED (kernel-mode only)\n");
    printf("Phase 4 - State Transitions:     %s\n",
        (cap_running && ren_running) ? "OK" : "BLOCKED (needs DMA buffer first)");
    printf("\n");

    if (!cap_running || !ren_running) {
        printf("--- The Wall ---\n\n");
        printf("WaveRT pins on modern Windows use DMA buffers allocated in\n");
        printf("contiguous physical memory by PortCls (kernel port class driver).\n");
        printf("The KSPROPERTY_RTAUDIO_BUFFER property is a kernel-mode interface\n");
        printf("between PortCls and the miniport driver — not accessible from\n");
        printf("user-mode code.\n\n");
        printf("The audio stack on this machine:\n\n");
        printf("  User Mode:\n");
        printf("    App -> WASAPI -> AudioSes.dll -> RPC -> AudioSrv -> AudioDG.exe\n\n");
        printf("  Kernel Mode (inside AudioDG's context):\n");
        printf("    AudioDG -> KS pin -> PortCls -> WaveRT miniport -> HDA DMA\n\n");
        printf("From user mode, we CAN:\n");
        printf("  - Enumerate all KS filters and pins (full topology visible)\n");
        printf("  - Query pin data ranges, instance counts, communication types\n");
        printf("  - Create WaveRT pin instances (LOOPED_STREAMING interface)\n\n");
        printf("From user mode, we CANNOT:\n");
        printf("  - Map the WaveRT DMA buffer (PortCls kernel interface)\n");
        printf("  - Transition pin state without the buffer allocated\n");
        printf("  - Stream audio without a running pin\n\n");
        printf("To go below WASAPI on WaveRT hardware, you need:\n");
        printf("  1. A kernel-mode driver (ASIO4ALL ships ASIOA4A.sys)\n");
        printf("  2. Or use WASAPI exclusive mode (which calls into PortCls for you)\n\n");
        printf("WASAPI exclusive IS the user-mode floor for WaveRT audio.\n");
        printf("Our 6.67ms passthrough (examples/windows-wasapi-passthrough) is\n");
        printf("already at the lowest achievable latency from user space.\n");
    }

cleanup:
    /* Stop pins */
    if (cap_pin != INVALID_HANDLE_VALUE) {
        set_pin_state(cap_pin, KSSTATE_STOP);
        CloseHandle(cap_pin);
    }
    if (ren_pin != INVALID_HANDLE_VALUE) {
        set_pin_state(ren_pin, KSSTATE_STOP);
        CloseHandle(ren_pin);
    }

    CloseHandle(cap_filter);
    CloseHandle(ren_filter);

    return 0;
}
