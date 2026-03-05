# Building a Custom Kernel-Mode ASIO Driver for a Laptop Codec

**The golden artifact: direct DMA audio without ASIO4ALL.**

---

## TL;DR

We wrote a custom kernel driver + ASIO DLL pair that gives our Conexant CX20753/4 laptop codec direct DMA buffer access, bypassing both WASAPI and ASIO4ALL. The kernel driver is a minimal bridge (~450 lines) that does exactly ONE thing: allocate the WaveRT DMA buffer that requires kernel privileges. Everything else runs in user mode.

**Target: 3.33ms per buffer (160 frames at 48kHz) — the hardware minimum.**

## Why This Exists

We ran three experiments in sequence, each going deeper:

| Experiment | Layer | Result |
|-----------|-------|--------|
| [WASAPI passthrough](../windows-wasapi-passthrough/) | WASAPI Exclusive | 6.67ms round-trip (3.33ms per buffer) |
| [KS passthrough](../windows-ks-passthrough/) | WDM Kernel Streaming | Hit the wall: DMA buffer alloc is kernel-only |
| **This project** | Custom kernel driver | Direct DMA access, no middleware |

ASIO4ALL exists to solve this problem, but it's generic — it discovers devices at runtime, handles every codec, every format, every edge case. Our driver is purpose-built for one specific codec with hardcoded parameters:

| Parameter | Value | Why |
|-----------|-------|-----|
| Codec | Conexant CX20753/4 | Our specific hardware |
| Bus | Intel HDA | The only bus this codec uses |
| Format | 16-bit PCM stereo | Codec's native exclusive format |
| Sample rate | 48000 Hz | Codec's native rate |
| Buffer | 160 frames (3.33ms) | HDA DMA alignment minimum |

Zero runtime discovery. Zero format negotiation. Zero abstraction.

## The Architecture

```
User Mode (ASIO DLL):
  DAW / Test App
    → CoCreateInstance(CLSID_HdaDirectAsio)
    → ASIO init: enumerate devices, open HDA filter, create KS pins
    → IOCTL to kernel bridge: allocate DMA buffer
    → DMA buffer mapped directly into app's address space
    → Hot path: read/write DMA buffer (zero-copy, zero-syscall)

Kernel Mode (bridge driver):
  hda_bridge.sys
    → Receives pin handle from user mode
    → Calls KSPROPERTY_RTAUDIO_BUFFER (THE kernel-only operation)
    → Maps DMA buffer MDL into user process
    → Returns user-mode pointer to physically contiguous DMA memory

Hardware:
  Intel HDA Controller → Conexant CX20753/4 Codec
    → DMA engine reads/writes the same physical memory
    → ADC samples appear in capture buffer automatically
    → DAC reads from render buffer automatically
```

**The hot path has zero system calls.** The DMA buffer is in physically contiguous memory, mapped to both the hardware DMA engine and the user process. The hardware writes ADC samples and reads DAC samples via DMA. The app reads and writes the buffer via a normal pointer. No copies. No context switches.

## What the Kernel Driver Actually Does

The kernel driver is ~450 lines of C. It handles exactly 4 IOCTLs:

| IOCTL | Purpose | Why it needs kernel mode |
|-------|---------|------------------------|
| `ALLOC_RT_BUFFER` | Allocate WaveRT DMA buffer | PortCls checks requestor mode |
| `FREE_RT_BUFFER` | Unmap and release buffer | Cleanup mapped MDL pages |
| `SET_PIN_STATE` | Transition STOP→RUN | May require kernel context on WaveRT |
| `GET_RT_POSITION` | Map hardware position register | Hardware BAR mapping |

That's it. No device enumeration. No pin creation. No format negotiation. All of that works from user mode (we proved it in the KS experiment). The kernel driver is purely a privilege bridge for the DMA buffer.

## Why Not ASIO4ALL?

| | ASIO4ALL | HDA Direct ASIO |
|---|---------|----------------|
| **Discovery** | Scans all audio devices at runtime | Hardcoded to CX20753/4 |
| **Format** | Negotiates format per device | Hardcoded: PCM 16/48k |
| **Buffer** | Calculated per device | Hardcoded: 160 frames |
| **Code path** | Generic KS property queries | Direct DMA buffer access |
| **Driver** | `ASIOA4A.sys` (~100KB, generic) | `hda_bridge.sys` (~10KB, minimal) |
| **Latency** | Good (~5-10ms) | Hardware minimum (3.33ms) |
| **Purpose** | Universal compatibility | Maximum performance on one device |

## Components

```
windows-hda-asio/
├── driver/
│   └── hda_bridge.c          # Kernel bridge driver (WDK required)
├── dll/
│   ├── hda_asio.c            # ASIO DLL (COM in-process server)
│   └── hda_asio.def          # DLL exports
├── test/
│   └── asio_loopback.c       # Standalone mic→speaker test
├── include/
│   ├── hda_bridge_ioctl.h    # Shared IOCTL definitions
│   └── asio.h                # ASIO interface types
└── tools/
    ├── build_driver.bat      # Build with WDK/EWDK
    ├── build_dll.bat         # Build with MinGW
    ├── build_test.bat        # Build test app
    ├── install.ps1           # Install everything (admin)
    └── uninstall.ps1         # Clean removal (admin)
```

## Building

**Everything builds with MinGW (w64devkit). No WDK, no Visual Studio, no 15GB downloads.**

w64devkit ships with DDK headers (`ntddk.h`, `wdm.h`, `ks.h`), kernel import libraries (`libntoskrnl.a`, `libhal.a`, `libks.a`), and KS GUID data (`libksguid.a`). That's everything needed to compile a kernel driver.

### Prerequisites

| Component | Tool | Size | Notes |
|-----------|------|------|-------|
| All three | [w64devkit](https://github.com/skeeto/w64devkit) | 37MB | GCC 15.2.0 + DDK headers |

### Build commands

```bash
cd tools

# Build kernel driver (.sys)
build_driver.bat

# Build ASIO DLL
build_dll.bat

# Build test app
build_test.bat
```

Or build manually:

```bash
# Kernel driver
gcc -nostdlib -nostartfiles -shared -Wall \
    -o hda_bridge.sys driver/hda_bridge.c \
    -I<w64devkit>/include/ddk \
    -Wl,--subsystem,native -Wl,--entry,DriverEntry \
    -lntoskrnl -lhal -lks -lksguid

# ASIO DLL
gcc -shared -O2 -Wall -o hda_asio.dll dll/hda_asio.c dll/hda_asio.def \
    -lole32 -loleaut32 -lsetupapi -lksuser -lavrt -luuid

# Test app
gcc -O2 -Wall -o asio_loopback.exe test/asio_loopback.c -lole32 -loleaut32
```

## Installing

**CRITICAL: Kernel drivers can crash your system. This requires test signing mode.**

```powershell
# Run as Administrator
# Step 1: Enable test signing (one-time, requires reboot)
bcdedit /set testsigning on
# REBOOT

# Step 2: Install everything
Start-Process powershell -Verb RunAs -ArgumentList '-File','C:\path\to\install.ps1'

# Step 3: Test
asio_loopback.exe
```

### Manual install:

```cmd
REM Install driver
sc create HdaAsioBridge type= kernel binPath= "C:\path\to\hda_bridge.sys"
sc start HdaAsioBridge

REM Register ASIO DLL
regsvr32 hda_asio.dll

REM Test
asio_loopback.exe
```

## Using in FL Studio

1. Install the driver and DLL (see above)
2. Open FL Studio
3. Options → Audio Settings
4. Select **HDA Direct ASIO** from the ASIO driver dropdown
5. Buffer should show 160 samples / 3.33ms

## The Journey

### Experiment 1: WASAPI Exclusive (6.67ms)

We proved that WASAPI exclusive mode gives us 160-frame buffers at 48kHz — the HDA codec's minimum. But WASAPI still makes kernel calls on our behalf through AudioDG.exe.

### Experiment 2: Raw KS (hit the wall)

We wrote a diagnostic tool that talks directly to the KS filter graph. We could see everything — filter topology, pin formats, instance counts — and even create WaveRT pin instances. But WaveRT DMA buffer allocation (`KSPROPERTY_RTAUDIO_BUFFER`) returned ERROR_NOT_FOUND from user mode. PortCls checks requestor mode.

### Experiment 3: This project (through the wall)

Instead of a generic driver, we wrote a minimal kernel bridge that does exactly one thing: call `KSPROPERTY_RTAUDIO_BUFFER` from kernel context and map the result to user mode. The ASIO DLL handles everything else from user mode.

The result: direct DMA buffer access. The app writes samples to physically contiguous memory that the HDA controller's DMA engine reads from. No copies. No intermediate buffers. No Audio Engine. No AudioDG.

## Key Technical Details

- **WaveRT LOOPED_STREAMING**: The pin interface for modern HDA audio. Creates a circular DMA buffer that the hardware streams continuously.
- **MDL mapping**: The kernel driver uses `MmMapLockedPagesSpecifyCache` to map the DMA buffer's physical pages into the user process's virtual address space.
- **Position register**: WaveRT can expose a hardware register that the DMA engine updates with the current byte offset. When mapped to user mode, position tracking requires zero syscalls.
- **MMCSS**: The streaming thread uses `AvSetMmThreadCharacteristics("Pro Audio")` for real-time scheduling priority.
- **Test signing**: Required for loading unsigned kernel drivers on Windows 10+. Adds a watermark to the desktop.

## Safety Notes

**Kernel drivers can BSOD your system.** This code is experimental.

- Always test on a non-production machine first
- The driver is minimal (~450 lines) to reduce risk surface
- All error paths clean up properly (MDLs freed, references released)
- The driver unloads cleanly via `sc stop HdaAsioBridge`
- Remove with `uninstall.ps1` to clean up everything

---

*This is a [signal-chain](https://github.com/ELI7VH/signal-chain) experiment — proving that device-specific kernel drivers can outperform generic audio middleware.*
