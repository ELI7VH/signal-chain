# How Deep Can You Go? WDM Kernel Streaming from User Mode

**We tried to go below WASAPI. Here's what we found.**

---

## TL;DR

We wrote a C program that talks directly to the WDM Kernel Streaming layer -- the driver interface below WASAPI. We got deep: opened the HDA filter graph, queried every pin's formats and topology, and even created WaveRT pin instances. Then we hit **the wall**: WaveRT DMA buffer allocation is a kernel-mode-only operation. You can't stream audio without it.

**WASAPI exclusive mode is the user-mode floor for audio on modern Windows.**

## The Audio Stack

```
App -> WASAPI Shared -> Audio Engine -> KS -> Driver -> Hardware   (shared mode)
App -> WASAPI Exclusive -> KS -> Driver -> Hardware                (exclusive mode)
App -> KS Direct -> ???                                            (this experiment)
```

We proved with the [WASAPI passthrough](../windows-wasapi-passthrough/) that exclusive mode gets 6.67ms round-trip on our laptop. But WASAPI still calls into the kernel on our behalf. What if we cut it out entirely?

## What We Built

A KS filter/pin diagnostic tool that:

1. **Enumerates** all KS audio device interfaces (KSCATEGORY_AUDIO, RENDER, CAPTURE)
2. **Opens** the HDA wave filter handles directly via CreateFile
3. **Inspects** every pin: data flow direction, communication type, supported formats, instance counts
4. **Creates** WaveRT pin instances with LOOPED_STREAMING interface
5. **Attempts** to allocate the RT DMA buffer and transition pin states

## What We Found

### Phase 1-2: Full Visibility

From user mode, we can see the **entire KS filter graph** of the HDA audio device:

```
KSCATEGORY_AUDIO (9 devices):
  root#media#0001 -> topology, wave         (software audio engine)
  hdaudio#...     -> espeakertopo           (speaker mixer/routing)
  hdaudio#...     -> emicintopo             (mic mixer/routing)
  hdaudio#...     -> eheadphonetopo         (headphone mixer/routing)
  hdaudio#...     -> espeakerwave           (speaker PCM transport)
  hdaudio#...     -> emicinwave             (mic PCM transport)
  hdaudio#...     -> eheadphonewave         (headphone PCM transport)
```

Each wave filter exposes pins with queryable properties:
- **Data ranges:** PCM, 1-2ch, 44100-96000Hz (capture), 44100-192000Hz (render), 16-24 bit
- **Instance counts:** max=1 per pin, current=0 (AudioDG releases when not streaming)
- **Communication:** SINK (connectable from user mode)

### Phase 3: Pin Creation Works

KsCreatePin succeeds with `KSINTERFACE_STANDARD_LOOPED_STREAMING`. This confirms the HDA driver uses WaveRT (the modern port model for real-time audio on Windows).

Key discovery: `KSINTERFACE_STANDARD_STREAMING` (the old WaveCyclic interface) returns ERROR_NO_MATCH. The generic HDA driver is WaveRT-only.

### Phase 4: The Wall

**WaveRT DMA buffer allocation (`KSPROPERTY_RTAUDIO_BUFFER`) is kernel-mode only.**

```
KSPROPERTY_RTAUDIO_BUFFER -> ERROR_NOT_FOUND (1168)
KSPROPERTY_CONNECTION_STATE -> ERROR_BAD_COMMAND (22)
```

The buffer lives in contiguous physical memory, allocated by PortCls (the kernel-mode port class driver) and mapped into the caller's address space. Since the caller is normally AudioDG.exe (running in a protected process context), user-mode apps can't request this mapping.

Without the DMA buffer, the pin can't transition from STOP to ACQUIRE, and without ACQUIRE there's no streaming.

## The Architecture

```
User Mode:
  App -> WASAPI -> AudioSes.dll -> RPC -> AudioSrv -> AudioDG.exe

Kernel Mode (inside AudioDG's context):
  AudioDG -> KS pin -> PortCls -> WaveRT miniport -> HDA DMA buffer

The DMA buffer is in contiguous physical memory, mapped into AudioDG's
address space, then shared back to the app through WASAPI's buffer APIs.
```

## What You'd Need to Go Lower

| Approach | What it does | Example |
|----------|-------------|---------|
| **Kernel driver (.sys)** | Runs inside the kernel, can call PortCls directly | ASIO4ALL ships `ASIOA4A.sys` |
| **Signed miniport driver** | Replaces the HDA miniport entirely | Custom audio drivers |
| **WASAPI exclusive** | Calls into PortCls on your behalf from user mode | Our WASAPI passthrough |

ASIO4ALL achieves sub-WASAPI latency because its kernel driver creates KS pins, maps the WaveRT DMA buffer, and exposes it back to user mode through its own ASIO interface. It's a kernel-mode bridge.

## Building & Running

```bash
gcc -O2 -Wall -o ks_passthrough.exe src/ks_passthrough.c -lsetupapi -lksuser -lole32 -lavrt

./ks_passthrough.exe
```

No command-line options -- it's a diagnostic/exploration tool, not a streaming app.

## Key Takeaway

**WASAPI exclusive mode is not "another layer of abstraction" on top of KS. It IS the user-mode interface to KS.** Going below it requires kernel code. Our 6.67ms WASAPI exclusive passthrough is already at the hardware floor for user-mode audio on this machine.

The signal-chain thesis still holds -- but the "lowest possible layer" on Windows is WASAPI exclusive, not raw KS. The abstractions above WASAPI (shared mode, the audio engine) are where all the latency hides.

---

*This is a [signal-chain](https://github.com/ELI7VH/signal-chain) experiment -- mapping the exact boundaries of audio abstraction on Windows.*
