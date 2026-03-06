# HDA Direct ASIO — Custom WASAPI Exclusive Mode Driver

**A working ASIO driver for the Conexant CX20753/4 laptop codec, built from scratch with MinGW. No WDK. No Steinberg SDK. No ASIO4ALL.**

**6.67ms round-trip. 160 frames at 48kHz. Bit-perfect loopback. Tested in FL Studio and Studio One.**

---

## The Win

We wrote a custom ASIO driver that gives a laptop's built-in Conexant codec proper ASIO support via WASAPI exclusive mode. The driver:

- **Works in real DAWs** (FL Studio, Studio One — tested and verified)
- **Supports 44.1kHz and 48kHz** with live sample rate switching
- **Passes bit-perfect validation** (288,000 samples, zero mismatches)
- **Runs at hardware minimum latency** (160 frames / 3.33ms per buffer)
- **Built entirely with MinGW** (w64devkit, 37MB download)

## Why This Exists

Windows audio is a pain in the ass. The built-in codecs don't ship with ASIO drivers, and ASIO4ALL is generic — it discovers devices at runtime, handles every codec, every format. We wanted purpose-built.

| Parameter | Value |
|-----------|-------|
| Codec | Conexant CX20753/4 |
| Bus | Intel HDA (ASUS X510UAR) |
| Format | 16-bit PCM stereo |
| Sample rates | 44100 / 48000 Hz |
| Buffer | 160 frames (3.33ms @ 48k) |
| Round-trip | 6.67ms (buffer only) |
| Toolchain | MinGW (w64devkit) — no WDK, no MSVC |

## The Journey

We tried three approaches, each going deeper into the Windows audio stack:

### Experiment 1: WASAPI Passthrough (Success — 6.67ms)

Proved that WASAPI exclusive mode gives us 160-frame buffers at 48kHz — the codec's hardware minimum. Event-driven streaming with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.

### Experiment 2: KS Direct Path (Blocked)

Wrote a diagnostic tool that talks directly to the KS filter graph. Opened pins, read topology, created WaveRT instances. But the Conexant miniport driver permanently blocks state transitions from user mode (error 22). No RTAUDIO properties exposed. **Dead end.**

### Experiment 3: Kernel Bridge (Built, Then Abandoned)

Wrote a minimal kernel driver (`hda_bridge.sys`, ~450 lines) to call `KSPROPERTY_RTAUDIO_BUFFER` from kernel context. Built with MinGW — no WDK needed. The driver compiled and loaded, but the WaveRT DMA buffer wall was real: the miniport simply doesn't expose the DMA registers we need.

### Experiment 4: WASAPI-Backed ASIO (The Winner)

Wrapped WASAPI exclusive mode in a proper ASIO COM interface. This gives us:
- Same 160-frame hardware minimum latency as the raw WASAPI test
- Proper ASIO vtable that DAWs understand
- Event-driven streaming with MMCSS "Pro Audio" thread priority
- Per-channel non-interleaved buffers (the ASIO way)

**This is the version that ships.**

## Bugs Found & Fixed

| Bug | Symptom | Root Cause | Fix |
|-----|---------|------------|-----|
| Studio One crash on load | Crash dump, access violation | Missing `canSampleRate` in vtable — every method after slot 12 was off by one | Added `canSampleRate` at correct vtable position |
| Audio sounds terrible | Duplicate-pair pattern in recordings | WASAPI gives interleaved stereo, ASIO expects non-interleaved per-channel | Deinterleave capture, reinterleave render in streaming thread |
| CPU meter at 100% | Studio One reports max CPU | `outputReady` returning `ASE_OK` when driver doesn't support the optimization | Changed to return `ASE_NotPresent` |

## Architecture

```
DAW (FL Studio / Studio One / etc.)
  |
  v
ASIO Interface (COM in-process server)
  - hda_asio.dll (~1100 lines C)
  - Implements full IASIO vtable
  - Per-channel non-interleaved double buffers
  |
  v
WASAPI Exclusive Mode
  - Event-driven streaming (AUDCLNT_STREAMFLAGS_EVENTCALLBACK)
  - Separate capture + render IAudioClient
  - MMCSS "Pro Audio" thread priority
  |
  v
Hardware
  - Intel HDA Controller → Conexant CX20753/4
  - 160-frame DMA periods at 44.1k or 48kHz
```

## Components

```
windows-hda-asio/
├── dll/
│   ├── hda_asio.c            # ASIO DLL — WASAPI exclusive mode backend
│   └── hda_asio.def          # DLL exports
├── test/
│   ├── asio_loopback.c       # Mic → speaker loopback with stats
│   └── asio_validate.c       # Buffer capture + bit-perfect validation
├── include/
│   ├── asio.h                # ASIO interface types + vtable
│   └── hda_bridge_ioctl.h    # (legacy — kernel driver IOCTLs)
├── driver/
│   └── hda_bridge.c          # (legacy — kernel bridge, not used)
└── tools/
    ├── hda_asio.dll           # Built DLL
    ├── asio_loopback.exe      # Built test
    ├── asio_validate.exe      # Built validation test
    ├── install.ps1            # Install script (admin)
    └── uninstall.ps1          # Uninstall script (admin)
```

## Building

**Everything builds with MinGW (w64devkit). No WDK, no Visual Studio, no Steinberg SDK.**

```bash
export PATH="/path/to/w64devkit/bin:$PATH"
cd tools

# Build ASIO DLL
gcc -shared -O2 -Wall -Wno-unknown-pragmas -I../include \
    -o hda_asio.dll ../dll/hda_asio.c ../dll/hda_asio.def \
    -lole32 -loleaut32 -lavrt -luuid

# Build loopback test
gcc -O2 -Wall -o asio_loopback.exe ../test/asio_loopback.c -lole32 -loleaut32

# Build validation test
gcc -O2 -Wall -o asio_validate.exe ../test/asio_validate.c -lole32 -loleaut32
```

## Installing

```powershell
# Run as Administrator
cd tools
regsvr32 hda_asio.dll
```

Then open your DAW and select **HDA Direct ASIO** from the ASIO driver list.

## Test Results

### Loopback Test (asio_loopback.exe)
```
Driver: HDA Direct ASIO (v2)
Channels: 2 in, 2 out
Buffer: 160 frames (3.33ms)
Round-trip: 6.67ms
LIVE: 302 callbacks/sec, 48kHz confirmed
```

### Validation Test (asio_validate.exe)
```
Channel 0: PASS — 144,000 samples, bit-perfect match
Channel 1: PASS — 144,000 samples, bit-perfect match
RESULT: ALL CHANNELS BIT-PERFECT

Interleaving Check:
  Channel 0: OK — 11.8% duplicate pairs (normal for real audio)
  Channel 1: OK — 11.8% duplicate pairs (normal for real audio)
```

## Why Windows Is Such a Pain

The Windows audio stack has 4+ layers between your app and the hardware:

```
App → WASAPI → Audio Engine (AudioDG.exe) → WDM → Miniport → HDA Controller → Codec
```

ASIO exists because this stack adds too much latency for real-time audio. But writing an ASIO driver means implementing a COM server with a 20+ method vtable, and the spec is only available under NDA from Steinberg.

We reverse-engineered the vtable from open source implementations, built the COM factory from scratch in C, and debugged it against real DAWs. The `canSampleRate` vtable slot bug (one missing method shifts EVERY subsequent method pointer) is the kind of thing that makes you appreciate why people just use ASIO4ALL.

But now it works. And it's purpose-built. And it's fast.

---

*Part of [signal-chain](https://github.com/ELI7VH/signal-chain) — AI-generated custom audio orchestration for known hardware configurations.*
