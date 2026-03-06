# Audio Domain — Testing & Validation

Test patterns for audio driver development. Born from building a custom ASIO driver on Windows for the Conexant CX20753/4 HDA codec.

## Test Categories

### 1. Loopback Integrity

**What:** Capture mic input → copy to output buffer → verify bit-perfect match.

| Test | Tool | What It Proves |
|------|------|----------------|
| Per-channel loopback | `asio_loopback.exe` | Basic audio path works, callback fires at correct rate |
| Bit-perfect validation | `asio_validate.exe` | Input buffers == output buffers, zero sample loss |
| Interleave check | Built into validate | No interleaved data leaking into non-interleaved buffers |
| Duplicate-pair detection | Built into validate | Catches the WASAPI→ASIO deinterleave bug pattern |

### 2. Sample Rate Switching

**What:** Verify driver handles rate changes cleanly.

| Test | What to Check |
|------|---------------|
| Init at 48000 | Default path, should always work |
| Init at 44100 | WASAPI exclusive re-init with different format |
| Switch 48k→44.1k mid-session | `setSampleRate` tears down WASAPI, re-creates at new rate |
| Switch back 44.1k→48k | Round-trip rate change stability |
| Query `canSampleRate` for unsupported rates | Should return `ASE_NoClock` for 22050, 96000, etc. |

### 3. Buffer Geometry

**What:** Verify buffer sizes, alignment, and frame counts.

| Test | What to Check |
|------|---------------|
| WASAPI buffer alignment retry | `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` handled correctly |
| Buffer size matches `getBufferSize` | What WASAPI gives == what ASIO reports |
| Capture frames == render frames | Both endpoints at same period |
| Double-buffer swap | `bufferIndex` alternates 0→1→0→1 correctly |

### 4. DAW Integration

**What:** Load the driver in real DAWs and verify behavior.

| DAW | What to Check |
|-----|---------------|
| FL Studio | Driver appears in ASIO dropdown, audio records cleanly |
| Studio One | Driver loads, mic input works, CPU meter reads sane values |
| REAPER | ASIO driver detection, latency reporting matches |
| Ableton Live | Buffer size reported correctly, no crashes on load |

**Key DAW issues found during development:**
- **Studio One crash:** Missing `canSampleRate` in vtable shifted all method slots by 1
- **CPU at 100%:** `outputReady` returning `ASE_OK` when driver doesn't support it
- **Bad audio in all DAWs:** Interleaved WASAPI data in non-interleaved ASIO buffers

### 5. Timing & Performance

**What:** Verify real-time performance characteristics.

| Test | What to Check |
|------|---------------|
| Callback rate | Should be `sampleRate / bufferSize` callbacks/sec (e.g. 300/sec at 48k/160) |
| Total frames | `callbacks * bufferSize` should equal `elapsed * sampleRate` |
| MMCSS thread priority | Streaming thread should be in "Pro Audio" task group |
| Xrun detection | Count missed or late callbacks over extended runs |
| Round-trip latency | `inputLatency + outputLatency` in frames, verify with loopback measurement |

### 6. COM / ASIO Protocol

**What:** Verify the ASIO COM interface behaves correctly.

| Test | What to Check |
|------|---------------|
| `CoCreateInstance` | CLSID resolves, returns valid IASIO pointer |
| `init` / `Release` lifecycle | No leaks, proper cleanup on Release |
| Double `init` | Should return `ASIOTrue` immediately |
| `createBuffers` before `init` | Should return `ASE_NotPresent` |
| `start` before `createBuffers` | Should return `ASE_InvalidMode` |
| `disposeBuffers` while running | Should auto-stop first |
| `getChannelInfo` | Returns correct types (Int16LSB) and names |
| `getSamplePosition` | Monotonically increasing, timestamp in nanoseconds |

### 7. WAV Capture Analysis

**What:** Record audio through the driver and analyze the WAV files.

| Check | How | What Bad Looks Like |
|-------|-----|---------------------|
| Duplicate pairs | Compare `sample[i] == sample[i+1]` for >80% of pairs | Interleaving bug |
| DC offset | Mean sample value should be ~0 | Buffer init or silence handling issue |
| Frequency content | FFT should show expected range | Half sample rate = aliasing from interleave bug |
| Channel separation | L and R should differ for stereo source | Mono data in both channels = wrong buffer pointers |

## Test Artifacts

The `test/` directory contains:

| File | Purpose |
|------|---------|
| `asio_loopback.c` | Basic mic→speaker loopback with stats |
| `asio_validate.c` | Capture+validate: records buffers, compares, writes WAVs |

Both build with MinGW:

```bash
gcc -O2 -Wall -o asio_loopback.exe test/asio_loopback.c -lole32 -loleaut32
gcc -O2 -Wall -o asio_validate.exe test/asio_validate.c -lole32 -loleaut32
```

## Suggested Additional Tests

- **Stress test:** Run loopback for 1+ hour, count total xruns
- **Multi-client test:** Two apps try to load the driver simultaneously (WASAPI exclusive should reject the second)
- **Hot-unplug test:** Remove/disable audio device while streaming — should fail gracefully, not crash
- **Sleep/wake test:** Suspend system while streaming, resume, verify driver recovers
- **Format mismatch test:** Force a format the codec doesn't support, verify clean error
