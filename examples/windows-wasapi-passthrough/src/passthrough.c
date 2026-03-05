/*
 * signal-chain: WASAPI Audio Passthrough
 *
 * Zero-abstraction mic -> speaker loopback on Windows.
 * Tries exclusive mode with format probing, falls back to shared.
 *
 * Build:
 *   gcc -O2 -o passthrough.exe passthrough.c -lole32 -lksuser -lavrt
 *
 * Usage:
 *   passthrough.exe                    (defaults: 10ms buffer)
 *   passthrough.exe --buffer-ms 5      (buffer in milliseconds)
 *   passthrough.exe --buffer-frames 64 (buffer in samples)
 *   passthrough.exe --shared           (force shared mode)
 *
 * (c) 2026 Elijah Lucian / signal-chain
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ── Globals ────────────────────────────────────────────────────── */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── Helpers ────────────────────────────────────────────────────── */

#define CHECK_HR(hr, msg) do { \
    if (FAILED(hr)) { \
        fprintf(stderr, "FATAL: %s (hr=0x%08lX)\n", msg, (unsigned long)hr); \
        goto cleanup; \
    } \
} while(0)

static const char* format_name(WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) return "PCM";
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return "FLOAT";
    if (fmt->wFormatTag == 0xFFFE) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)fmt;
        if (IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) return "EXT/PCM";
        if (IsEqualGUID(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) return "EXT/FLOAT";
        return "EXT/OTHER";
    }
    return "UNKNOWN";
}

static void print_format(const char* label, WAVEFORMATEX* fmt) {
    printf("  %s: %s %dch %luHz %d-bit (align=%d, avgBPS=%lu)\n",
        label, format_name(fmt),
        fmt->nChannels, (unsigned long)fmt->nSamplesPerSec,
        fmt->wBitsPerSample, fmt->nBlockAlign,
        (unsigned long)fmt->nAvgBytesPerSec);
}

/*
 * Build a WAVEFORMATEXTENSIBLE struct for format probing.
 * This is what we send to IsFormatSupported / Initialize.
 */
static void build_format(WAVEFORMATEXTENSIBLE* wfx,
    int channels, int sample_rate, int bits, int is_float)
{
    memset(wfx, 0, sizeof(*wfx));
    wfx->Format.wFormatTag = 0xFFFE; /* WAVE_FORMAT_EXTENSIBLE */
    wfx->Format.nChannels = (WORD)channels;
    wfx->Format.nSamplesPerSec = (DWORD)sample_rate;
    wfx->Format.wBitsPerSample = (WORD)bits;
    wfx->Format.nBlockAlign = (WORD)(channels * bits / 8);
    wfx->Format.nAvgBytesPerSec = (DWORD)(sample_rate * wfx->Format.nBlockAlign);
    wfx->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx->Samples.wValidBitsPerSample = (WORD)bits;
    if (channels == 1)
        wfx->dwChannelMask = 0x4; /* SPEAKER_FRONT_CENTER */
    else
        wfx->dwChannelMask = 0x3; /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */

    if (is_float)
        wfx->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    else
        wfx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

/*
 * Probe a device for exclusive mode support with various formats.
 * Returns 1 if a format was found, 0 otherwise.
 * The winning format is written to *out_fmt.
 */
static int probe_exclusive_format(IAudioClient* client, WAVEFORMATEXTENSIBLE* out_fmt) {
    /* Formats to try, in preference order:
     * 1. 16-bit PCM stereo 48kHz  — most universally supported
     * 2. 16-bit PCM stereo 44.1kHz
     * 3. 24-bit PCM stereo 48kHz
     * 4. 32-bit float stereo 48kHz
     * 5. 32-bit PCM stereo 48kHz
     */
    struct { int bits; int is_float; int rate; } probes[] = {
        { 16, 0, 48000 },
        { 16, 0, 44100 },
        { 24, 0, 48000 },
        { 32, 1, 48000 },
        { 32, 0, 48000 },
        { 24, 0, 44100 },
        { 32, 1, 44100 },
    };
    int n_probes = sizeof(probes) / sizeof(probes[0]);

    for (int i = 0; i < n_probes; i++) {
        WAVEFORMATEXTENSIBLE wfx;
        build_format(&wfx, 2, probes[i].rate, probes[i].bits, probes[i].is_float);

        HRESULT hr = IAudioClient_IsFormatSupported(client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            (WAVEFORMATEX*)&wfx, NULL);

        printf("    Probe: %d-bit %s %dHz -> %s\n",
            probes[i].bits,
            probes[i].is_float ? "float" : "PCM",
            probes[i].rate,
            SUCCEEDED(hr) ? "SUPPORTED" : "no");

        if (SUCCEEDED(hr)) {
            *out_fmt = wfx;
            return 1;
        }
    }
    return 0;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    HRESULT hr;

    /* ── Parse arguments ────────────────────────────────────────── */
    int buffer_frames = 0;      /* 0 = use ms instead */
    int buffer_ms = 10;         /* default 10ms */
    int force_shared = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--buffer-frames") == 0 && i + 1 < argc) {
            buffer_frames = atoi(argv[++i]);
            if (buffer_frames < 16) buffer_frames = 16;
            if (buffer_frames > 16384) buffer_frames = 16384;
        } else if (strcmp(argv[i], "--buffer-ms") == 0 && i + 1 < argc) {
            buffer_ms = atoi(argv[++i]);
            if (buffer_ms < 1) buffer_ms = 1;
            if (buffer_ms > 500) buffer_ms = 500;
        } else if (strcmp(argv[i], "--shared") == 0) {
            force_shared = 1;
        } else {
            /* Legacy: bare number = ms */
            int val = atoi(argv[i]);
            if (val > 0) buffer_ms = val;
        }
    }

    printf("=== signal-chain: WASAPI Audio Passthrough ===\n");
    if (buffer_frames > 0)
        printf("Target buffer: %d frames\n", buffer_frames);
    else
        printf("Target buffer: %dms\n", buffer_ms);
    if (force_shared)
        printf("Mode: SHARED (forced)\n");
    printf("Press Ctrl+C to stop.\n\n");

    signal(SIGINT, signal_handler);

    /* ── COM pointers (NULL for cleanup) ────────────────────────── */
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* capture_device = NULL;
    IMMDevice* render_device = NULL;
    IAudioClient* capture_client = NULL;
    IAudioClient* render_client = NULL;
    IAudioCaptureClient* capture_svc = NULL;
    IAudioRenderClient* render_svc = NULL;
    WAVEFORMATEX* cap_mix_fmt = NULL;
    WAVEFORMATEX* ren_mix_fmt = NULL;
    HANDLE capture_event = NULL;
    HANDLE render_event = NULL;
    HANDLE task_handle = NULL;
    DWORD task_index = 0;

    /* The format we'll actually use */
    WAVEFORMATEXTENSIBLE active_fmt;
    WAVEFORMATEX* fmt = NULL;  /* points to active_fmt or mix format */
    int exclusive_mode = 0;

    /* ── Initialize COM ─────────────────────────────────────────── */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CHECK_HR(hr, "CoInitializeEx");

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &IID_IMMDeviceEnumerator, (void**)&enumerator);
    CHECK_HR(hr, "Create device enumerator");

    /* ── Get devices ────────────────────────────────────────────── */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        enumerator, eCapture, eConsole, &capture_device);
    CHECK_HR(hr, "Get capture device");

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        enumerator, eRender, eConsole, &render_device);
    CHECK_HR(hr, "Get render device");

    /* ── Activate clients ───────────────────────────────────────── */
    hr = IMMDevice_Activate(capture_device, &IID_IAudioClient,
        CLSCTX_ALL, NULL, (void**)&capture_client);
    CHECK_HR(hr, "Activate capture");

    hr = IMMDevice_Activate(render_device, &IID_IAudioClient,
        CLSCTX_ALL, NULL, (void**)&render_client);
    CHECK_HR(hr, "Activate render");

    /* ── Report mix formats ─────────────────────────────────────── */
    hr = IAudioClient_GetMixFormat(capture_client, &cap_mix_fmt);
    CHECK_HR(hr, "Get capture mix format");
    hr = IAudioClient_GetMixFormat(render_client, &ren_mix_fmt);
    CHECK_HR(hr, "Get render mix format");

    printf("Device mix formats (what the driver reports):\n");
    print_format("Capture", cap_mix_fmt);
    print_format("Render ", ren_mix_fmt);
    printf("\n");

    /* ── Try exclusive mode with format probing ─────────────────── */
    if (!force_shared) {
        printf("Probing capture for exclusive mode formats...\n");
        int cap_ok = probe_exclusive_format(capture_client, &active_fmt);

        if (cap_ok) {
            printf("\nProbing render for same format...\n");
            /* Check if render also supports the same format */
            HRESULT ren_hr = IAudioClient_IsFormatSupported(render_client,
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                (WAVEFORMATEX*)&active_fmt, NULL);

            if (SUCCEEDED(ren_hr)) {
                printf("  Render supports capture format: YES\n\n");

                /*
                 * Get the device's minimum period — this is the
                 * smallest REFERENCE_TIME the HDA controller accepts.
                 * Using this directly avoids frame↔time rounding errors
                 * that cause AUDCLNT_E_INVALID_DEVICE_PERIOD.
                 */
                REFERENCE_TIME cap_default_period, cap_min_period;
                IAudioClient_GetDevicePeriod(capture_client, &cap_default_period, &cap_min_period);

                REFERENCE_TIME buf_dur;
                if (buffer_frames > 0) {
                    buf_dur = (REFERENCE_TIME)(10000000LL * buffer_frames / active_fmt.Format.nSamplesPerSec);
                } else {
                    buf_dur = (REFERENCE_TIME)(buffer_ms * 10000LL);
                }

                /* Clamp to device minimum */
                if (buf_dur < cap_min_period) {
                    printf("  Requested %.2fms, device minimum is %.2fms. Using minimum.\n",
                        (double)buf_dur / 10000.0,
                        (double)cap_min_period / 10000.0);
                    buf_dur = cap_min_period;
                }

                /* Align to device period (must be integer multiple) */
                if (cap_min_period > 0 && (buf_dur % cap_min_period) != 0) {
                    buf_dur = ((buf_dur / cap_min_period) + 1) * cap_min_period;
                    printf("  Aligned buffer to %.2fms (device period boundary)\n",
                        (double)buf_dur / 10000.0);
                }

                UINT32 expected_frames = (UINT32)((buf_dur * active_fmt.Format.nSamplesPerSec + 5000000LL) / 10000000LL);
                printf("  Attempting exclusive: %u frames (%.2fms)...\n",
                    expected_frames, (double)buf_dur / 10000.0);

                hr = IAudioClient_Initialize(capture_client,
                    AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    buf_dur, buf_dur,
                    (WAVEFORMATEX*)&active_fmt, NULL);

                /* BUFFER_SIZE_NOT_ALIGNED: device tells us the right size */
                if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 aligned = 0;
                    IAudioClient_GetBufferSize(capture_client, &aligned);
                    if (aligned > 0) {
                        buf_dur = (REFERENCE_TIME)(10000000.0 * aligned / active_fmt.Format.nSamplesPerSec + 0.5);
                        printf("  Alignment fix: %u frames (%.2fms)\n",
                            aligned, (double)aligned / active_fmt.Format.nSamplesPerSec * 1000.0);
                    }
                    IAudioClient_Release(capture_client);
                    capture_client = NULL;
                    hr = IMMDevice_Activate(capture_device, &IID_IAudioClient,
                        CLSCTX_ALL, NULL, (void**)&capture_client);
                    CHECK_HR(hr, "Re-activate capture for alignment retry");

                    hr = IAudioClient_Initialize(capture_client,
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        buf_dur, buf_dur,
                        (WAVEFORMATEX*)&active_fmt, NULL);
                    if (FAILED(hr)) {
                        printf("  Aligned retry still failed (0x%08lX)\n", (unsigned long)hr);
                    }
                }

                if (SUCCEEDED(hr)) {
                    /* Use same period that worked for capture —
                     * same HDA codec, same alignment requirements */
                    REFERENCE_TIME ren_dur = buf_dur;
                    printf("  Render: using capture period (%u frames)\n",
                        (UINT32)((ren_dur * active_fmt.Format.nSamplesPerSec + 5000000LL) / 10000000LL));

                    hr = IAudioClient_Initialize(render_client,
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        ren_dur, ren_dur,
                        (WAVEFORMATEX*)&active_fmt, NULL);

                    /* Same alignment dance for render */
                    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                        UINT32 aligned = 0;
                        IAudioClient_GetBufferSize(render_client, &aligned);
                        if (aligned > 0) {
                            ren_dur = (REFERENCE_TIME)(10000000.0 * aligned / active_fmt.Format.nSamplesPerSec + 0.5);
                            printf("  Render alignment fix: %u frames (%.2fms)\n",
                                aligned, (double)aligned / active_fmt.Format.nSamplesPerSec * 1000.0);
                        }
                        IAudioClient_Release(render_client);
                        render_client = NULL;
                        hr = IMMDevice_Activate(render_device, &IID_IAudioClient,
                            CLSCTX_ALL, NULL, (void**)&render_client);
                        CHECK_HR(hr, "Re-activate render for alignment retry");

                        hr = IAudioClient_Initialize(render_client,
                            AUDCLNT_SHAREMODE_EXCLUSIVE,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            ren_dur, ren_dur,
                            (WAVEFORMATEX*)&active_fmt, NULL);
                    }

                    if (SUCCEEDED(hr)) {
                        exclusive_mode = 1;
                        fmt = (WAVEFORMATEX*)&active_fmt;
                        printf("EXCLUSIVE MODE ACTIVE\n");
                    } else {
                        printf("  Render init failed (0x%08lX)\n", (unsigned long)hr);
                        IAudioClient_Release(capture_client);
                        capture_client = NULL;
                        IAudioClient_Release(render_client);
                        render_client = NULL;
                        hr = IMMDevice_Activate(capture_device, &IID_IAudioClient,
                            CLSCTX_ALL, NULL, (void**)&capture_client);
                        CHECK_HR(hr, "Re-activate capture");
                        hr = IMMDevice_Activate(render_device, &IID_IAudioClient,
                            CLSCTX_ALL, NULL, (void**)&render_client);
                        CHECK_HR(hr, "Re-activate render");
                    }
                } else {
                    printf("  Capture init failed (0x%08lX)\n", (unsigned long)hr);
                    IAudioClient_Release(capture_client);
                    capture_client = NULL;
                    hr = IMMDevice_Activate(capture_device, &IID_IAudioClient,
                        CLSCTX_ALL, NULL, (void**)&capture_client);
                    CHECK_HR(hr, "Re-activate capture");
                }
            } else {
                printf("  Render doesn't support capture format\n");
            }
        } else {
            printf("  No exclusive formats found for capture.\n");
        }
    }

    /* ── Shared mode fallback ───────────────────────────────────── */
    if (!exclusive_mode) {
        if (!force_shared)
            printf("\nFalling back to shared mode.\n");

        fmt = cap_mix_fmt;  /* use the mix format for shared */

        REFERENCE_TIME buf_dur;
        if (buffer_frames > 0)
            buf_dur = (REFERENCE_TIME)(10000000LL * buffer_frames / fmt->nSamplesPerSec);
        else
            buf_dur = (REFERENCE_TIME)(buffer_ms * 10000LL);

        hr = IAudioClient_Initialize(capture_client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            buf_dur, 0, cap_mix_fmt, NULL);
        CHECK_HR(hr, "Init capture shared");

        hr = IAudioClient_Initialize(render_client,
            AUDCLNT_SHAREMODE_SHARED,
            0, buf_dur, 0, ren_mix_fmt, NULL);
        CHECK_HR(hr, "Init render shared");
    }

    /* ── Event handles ──────────────────────────────────────────── */
    capture_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(capture_client, capture_event);
    CHECK_HR(hr, "Set capture event");

    if (exclusive_mode) {
        render_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        hr = IAudioClient_SetEventHandle(render_client, render_event);
        CHECK_HR(hr, "Set render event");
    }

    /* ── Report actual buffer sizes ─────────────────────────────── */
    UINT32 cap_buf, ren_buf;
    hr = IAudioClient_GetBufferSize(capture_client, &cap_buf);
    CHECK_HR(hr, "Get capture buffer size");
    hr = IAudioClient_GetBufferSize(render_client, &ren_buf);
    CHECK_HR(hr, "Get render buffer size");

    double cap_ms = (double)cap_buf / fmt->nSamplesPerSec * 1000.0;
    double ren_ms = (double)ren_buf / fmt->nSamplesPerSec * 1000.0;

    printf("\n--- Active Configuration ---\n");
    printf("Mode:    %s\n", exclusive_mode ? "EXCLUSIVE (direct hardware)" : "SHARED (via audio engine)");
    print_format("Format", fmt);
    printf("Capture: %u frames (%.2fms)\n", cap_buf, cap_ms);
    printf("Render:  %u frames (%.2fms)\n", ren_buf, ren_ms);
    printf("Round-trip: %.2fms (buffer only, +hardware ADC/DAC)\n\n", cap_ms + ren_ms);

    /* ── Get services ───────────────────────────────────────────── */
    hr = IAudioClient_GetService(capture_client,
        &IID_IAudioCaptureClient, (void**)&capture_svc);
    CHECK_HR(hr, "Get capture service");

    hr = IAudioClient_GetService(render_client,
        &IID_IAudioRenderClient, (void**)&render_svc);
    CHECK_HR(hr, "Get render service");

    /* ── MMCSS boost ────────────────────────────────────────────── */
    task_handle = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
    if (task_handle)
        printf("Thread: Pro Audio (MMCSS)\n");
    else
        printf("Warning: MMCSS boost failed\n");

    /* ── Start ──────────────────────────────────────────────────── */
    hr = IAudioClient_Start(capture_client);
    CHECK_HR(hr, "Start capture");
    hr = IAudioClient_Start(render_client);
    CHECK_HR(hr, "Start render");

    printf("LIVE — mic -> speakers [Ctrl+C to stop]\n\n");

    /* ── Audio loop ─────────────────────────────────────────────── */
    UINT64 total_frames = 0;
    UINT32 xruns = 0;

    while (g_running) {
        DWORD wait = WaitForSingleObject(capture_event, 100);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0) break;

        UINT32 packet_length;
        hr = IAudioCaptureClient_GetNextPacketSize(capture_svc, &packet_length);
        if (FAILED(hr)) break;

        while (packet_length > 0 && g_running) {
            BYTE* cap_data;
            UINT32 num_frames;
            DWORD flags;

            hr = IAudioCaptureClient_GetBuffer(capture_svc,
                &cap_data, &num_frames, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            int silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT);

            UINT32 padding = 0;
            if (!exclusive_mode)
                IAudioClient_GetCurrentPadding(render_client, &padding);

            UINT32 available = ren_buf - padding;
            UINT32 to_write = (num_frames < available) ? num_frames : available;

            if (to_write > 0) {
                BYTE* ren_data;
                hr = IAudioRenderClient_GetBuffer(render_svc, to_write, &ren_data);
                if (SUCCEEDED(hr)) {
                    if (silent)
                        memset(ren_data, 0, (size_t)to_write * fmt->nBlockAlign);
                    else
                        memcpy(ren_data, cap_data, (size_t)to_write * fmt->nBlockAlign);
                    IAudioRenderClient_ReleaseBuffer(render_svc, to_write, 0);
                    total_frames += to_write;
                } else {
                    xruns++;
                }
            } else {
                xruns++;
            }

            IAudioCaptureClient_ReleaseBuffer(capture_svc, num_frames);
            hr = IAudioCaptureClient_GetNextPacketSize(capture_svc, &packet_length);
            if (FAILED(hr)) break;
        }

        /* Status every ~2s */
        if (total_frames > 0 &&
            (total_frames % (fmt->nSamplesPerSec * 2)) < cap_buf)
        {
            double secs = (double)total_frames / fmt->nSamplesPerSec;
            printf("\r  %.0fs | %llu frames | %u xruns   ",
                secs, (unsigned long long)total_frames, xruns);
            fflush(stdout);
        }
    }

    printf("\n\nStopped.\n");
    printf("Total: %llu frames (%.1fs)\n",
        (unsigned long long)total_frames,
        (double)total_frames / fmt->nSamplesPerSec);
    printf("Xruns: %u\n", xruns);

cleanup:
    if (capture_client) IAudioClient_Stop(capture_client);
    if (render_client) IAudioClient_Stop(render_client);
    if (task_handle) AvRevertMmThreadCharacteristics(task_handle);
    if (capture_svc) IAudioCaptureClient_Release(capture_svc);
    if (render_svc) IAudioRenderClient_Release(render_svc);
    if (capture_client) IAudioClient_Release(capture_client);
    if (render_client) IAudioClient_Release(render_client);
    if (capture_device) IMMDevice_Release(capture_device);
    if (render_device) IMMDevice_Release(render_device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (cap_mix_fmt) CoTaskMemFree(cap_mix_fmt);
    if (ren_mix_fmt) CoTaskMemFree(ren_mix_fmt);
    if (capture_event) CloseHandle(capture_event);
    if (render_event) CloseHandle(render_event);
    CoUninitialize();
    return 0;
}
