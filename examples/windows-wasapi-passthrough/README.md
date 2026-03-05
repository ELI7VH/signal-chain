# Building a Zero-Abstraction Audio Passthrough on Windows

**The signal-chain way: know the hardware, skip every layer you don't need.**

---

## TL;DR

We wrote a C program that routes mic to speakers with **6.67ms round-trip latency** using WASAPI exclusive mode on a laptop with no external audio interface. Along the way we swapped out the OEM driver, fought HDA buffer alignment, and learned exactly what sits between your code and the ADC/DAC.

**Final result:**
```
Mode:    EXCLUSIVE (direct hardware)
Format:  16-bit PCM stereo 48kHz
Capture: 160 frames (3.33ms)
Render:  160 frames (3.33ms)
Round-trip: 6.67ms (buffer only, +hardware ADC/DAC)
Xruns: 0
```

## The Setup

| | |
|---|---|
| **Machine** | ASUS X510UAR laptop (Intel i7-8550U, 16GB RAM) |
| **Audio** | Conexant CX20753/4 codec on Intel HDA bus |
| **OS** | Windows 11 |
| **Goal** | Mic to headphone jack, lowest possible latency, in C |

No DAW. No ASIO4ALL. No Python wrapper. Just a C program talking directly to the hardware through Windows Core Audio.

## Why C? Why WASAPI Exclusive?

Windows audio has layers:

```
App -> WASAPI Shared -> Audio Engine (mixing, format conversion, resampling) -> Driver -> Hardware
```

Every layer adds latency. WASAPI **shared mode** goes through the Windows Audio Engine. It mixes your stream with system sounds, converts formats, resamples if needed. That's 10-30ms of overhead you can never get back.

WASAPI **exclusive mode** skips all of it:

```
App -> WASAPI Exclusive -> Driver -> Hardware (direct DMA)
```

Your app gets exclusive access to the audio device. No mixing. No conversion. No resampling. The buffer goes straight to the hardware via DMA at its native format and sample rate.

We're writing it in C because:
1. No runtime overhead (no GC, no JVM, no event loop)
2. Direct COM vtable calls -- the thinnest possible wrapper over the kernel
3. This is what signal-chain would generate as a build artifact

## Step 1: Getting a Compiler

Windows doesn't ship with gcc. Visual Studio is 8GB of IDE. We grabbed [w64devkit](https://github.com/skeeto/w64devkit) -- a self-contained MinGW-w64 distribution in a 37MB download.

```
GCC 15.2.0 (w64devkit v2.5.0)
```

One-shot compile command:
```bash
PATH="/c/Users/elija/w64devkit/bin:$PATH" \
  gcc -O2 -Wall -o passthrough.exe src/passthrough.c -lole32 -lksuser -lavrt
```

The `-l` flags link COM (`ole32`), audio format GUIDs (`ksuser`), and the Multimedia Class Scheduler (`avrt`).

## Step 2: The Hardware Fingerprint

Before writing code, we audited the hardware. The Conexant codec reports:

- **Bus:** Intel HDA (High Definition Audio)
- **Device ID:** `HDAUDIO\FUNC_01&VEN_14F1&DEV_1F72`
- **Endpoints:** Speakers (render), Internal Microphone (capture)
- **ASIO drivers installed:** ASIO4ALL v2, FL Studio ASIO

Key insight: **we don't need ASIO.** WASAPI exclusive mode gives us the same direct hardware access that ASIO was invented to provide. ASIO4ALL is actually a wrapper around WDM Kernel Streaming -- WASAPI exclusive is cleaner and natively supported.

## Step 3: First Attempt -- Shared Mode Works

The first version compiled and ran in **shared mode**:

```
Mode:    SHARED (via audio engine)
Capture: 480 frames (21.33ms)
Render:  480 frames (24.00ms)
Round-trip: 45.33ms
Xruns: 0
```

It works, but 45ms is perceptible. You can hear the delay. We need exclusive mode.

## Step 4: The Conexant Wall

When we tried exclusive mode, every format probe came back `AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008)`:

```
Probe: 16-bit PCM 48000Hz -> no
Probe: 16-bit PCM 44100Hz -> no
Probe: 24-bit PCM 48000Hz -> no
Probe: 32-bit float 48000Hz -> no
...
No exclusive formats found for capture.
```

**The problem isn't the hardware.** The Conexant CX20753/4 is a standard HDA codec perfectly capable of exclusive mode. The problem is the **Conexant SmartAudio HD driver** -- an OEM driver that inserts a DSP pipeline (noise cancellation, EQ, spatial effects) between your app and the hardware. That DSP requires shared mode because it needs to process the audio before it hits the wire.

For audio production, that DSP is the enemy. Every processing step adds latency and changes the signal.

## Step 5: Swapping the Driver

The fix: replace the Conexant driver with Microsoft's generic **High Definition Audio Device** driver. This driver talks directly to the HDA codec with no DSP in the path.

**How to swap (reversible):**
1. Device Manager -> Sound, video and game controllers
2. Right-click "Conexant SmartAudio HD" -> Update driver
3. Browse my computer -> Let me pick from a list
4. Select **High Definition Audio Device**
5. Confirm the warning

After the swap, format probing lit up:

```
Probe: 16-bit PCM 48000Hz -> SUPPORTED
```

To restore the Conexant driver later: same steps, pick "Conexant SmartAudio HD" from the list.

## Step 6: The Buffer Alignment Gauntlet

With the generic driver, exclusive mode was *possible* but not *easy*. We hit three distinct WASAPI errors in sequence:

### Error 1: `AUDCLNT_E_INVALID_DEVICE_PERIOD (0x88890020)`

We asked for 64 frames at 48kHz. That's 1.33ms = 13333.33 hundred-nanoseconds. **Not an integer.** WASAPI's time unit (`REFERENCE_TIME`) is 100ns increments, and non-integer values are rejected.

**Fix:** Use `GetDevicePeriod()` to get the device's minimum period as an exact `REFERENCE_TIME`, then clamp our request to multiples of that period.

### Error 2: `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED (0x88890019)`

The device's minimum period (29000 hns = 2.90ms) maps to 139.2 frames at 48kHz. Not an integer frame count. The HDA DMA controller needs aligned transfers.

**Fix:** The WASAPI alignment retry pattern:
1. Call `Initialize()` -- it fails with `BUFFER_SIZE_NOT_ALIGNED`
2. Call `GetBufferSize()` on the *failed* client -- it returns the aligned frame count (160)
3. Recompute the `REFERENCE_TIME` from that aligned count
4. Release and re-create the client
5. Call `Initialize()` again with the corrected value

This is documented in MSDN but easy to miss. The device tells you the right answer after the first failure.

### Error 3: Render buffer mismatch

Capture initialized at 160 frames (3.33ms), but render kept landing at 1024 frames (21.33ms). Two problems:

1. We were querying render's `GetDevicePeriod()` separately, which returned the *default* period (~21ms), not the minimum
2. Render wasn't using `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, so WASAPI allocated a larger buffer

**Fix:** Use the same `REFERENCE_TIME` that worked for capture (same HDA codec = same alignment), and make render event-driven too. Both devices snapped to 160 frames.

## Step 7: Victory

```
=== signal-chain: WASAPI Audio Passthrough ===

--- Active Configuration ---
Mode:    EXCLUSIVE (direct hardware)
Format:  EXT/PCM 2ch 48000Hz 16-bit (align=4, avgBPS=192000)
Capture: 160 frames (3.33ms)
Render:  160 frames (3.33ms)
Round-trip: 6.67ms (buffer only, +hardware ADC/DAC)

Thread: Pro Audio (MMCSS)
LIVE -- mic -> speakers [Ctrl+C to stop]

  30s | 1440160 frames | 0 xruns
```

**6.67ms buffer round-trip, zero xruns, on a laptop with no external audio interface.**

The actual analog round-trip is higher (add ~1-2ms for ADC + DAC conversion), but sub-10ms is professional audio territory. On a $500 laptop.

## What We Learned

| Layer | Latency added |
|-------|---------------|
| WASAPI Shared mode | 20-45ms |
| Conexant DSP driver | blocks exclusive entirely |
| Generic HDA driver | 0ms overhead (direct DMA) |
| WASAPI Exclusive mode | 6.67ms (160-frame buffer) |
| MMCSS thread priority | prevents scheduling xruns |

**The signal-chain thesis holds:** most audio latency on consumer hardware comes from software abstraction, not hardware limitation. When you know the hardware and strip the unnecessary layers, a laptop codec performs like semi-pro gear.

## Key Technical Details

- **HDA minimum aligned buffer:** 160 frames at 48kHz (3.33ms). The codec's DMA controller dictates this -- you can't go lower without hardware changes.
- **Format:** 16-bit PCM stereo 48kHz. The Conexant codec's native exclusive mode format.
- **Event-driven mode** (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`) is essential. Without it, WASAPI may allocate larger buffers than requested.
- **MMCSS** (`AvSetMmThreadCharacteristics("Pro Audio")`) boosts thread scheduling priority. Without it, you'll get xruns under system load.

## Building & Running

```bash
# Compile (from this directory)
gcc -O2 -Wall -o passthrough.exe src/passthrough.c -lole32 -lksuser -lavrt

# Run with defaults (device minimum buffer)
./passthrough.exe

# Force a specific buffer size
./passthrough.exe --buffer-frames 160
./passthrough.exe --buffer-ms 5

# Force shared mode (for comparison)
./passthrough.exe --shared
```

**Requirements:**
- Windows 10/11
- MinGW-w64 or w64devkit (gcc)
- Generic HDA driver (not Conexant/Realtek OEM driver) for exclusive mode

## Source

- [`src/passthrough.c`](src/passthrough.c) -- the complete passthrough (~550 lines of C)
- [`hardware-fingerprint.json`](hardware-fingerprint.json) -- full hardware audit of the test machine

---

*This is a [signal-chain](https://github.com/ELI7VH/signal-chain) experiment -- proving that hardware-aware audio configuration can eliminate abstraction tax on any platform.*
