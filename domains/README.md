# Signal-Chain Domains

Testing patterns and validation suites organized by hardware domain. Each domain contains test categories, expected results, and known gotchas discovered during real development.

## Active Domains

| Domain | Status | Description |
|--------|--------|-------------|
| [audio](./audio/) | **Proven** | ASIO/WASAPI driver testing, buffer validation, DAW integration |

## Suggested Domains

These are natural extensions of signal-chain's "know your hardware, skip the abstraction" philosophy:

### USB (High Priority)

USB device communication — HID, serial, bulk transfer, isochronous (audio/video).

**Why:** USB audio interfaces are the #1 use case for signal-chain. Each device has specific endpoint configurations, native formats, and DMA transfer sizes. A USB domain would test:
- Endpoint enumeration and capability detection
- Format negotiation (S16_LE vs S32_LE per direction — the Volt 276 problem)
- Isochronous transfer timing and underrun detection
- Power management (USB selective suspend kills audio interfaces)
- Hot-plug/unplug resilience

### GPIO / Embedded (High Priority)

Raspberry Pi and embedded Linux hardware interfaces — SPI, I2C, UART, GPIO.

**Why:** signal-chain was born from a Pi build. GPIO testing would cover:
- Pin conflict detection (HyperPixer DPI vs DSI auto-detect)
- SPI/I2C bus speed validation for specific peripherals
- Display driver compatibility (DPI, DSI, HDMI)
- ALSA hw: vs plughw: format paths
- Real-time thread scheduling (PREEMPT_RT, CPU isolation)

### MIDI (Medium Priority)

MIDI protocol testing — USB MIDI, DIN MIDI, SysEx, clock.

**Why:** MIDI controllers are always part of an audio rig. Testing:
- USB MIDI enumeration and latency measurement
- SysEx message round-trip (LED feedback like Arturia MiniLab)
- MIDI clock jitter over time
- Multi-device routing (MIDI merge/split)
- Raw MIDI vs ALSA sequencer API

### Network (Medium Priority)

Network audio protocols — Dante, AES67, AVB, NDI.

**Why:** Pro audio increasingly uses network transport. Testing:
- PTP clock synchronization accuracy
- Packet jitter measurement
- Multicast group management
- Failover behavior
- Latency profiling across switch hops

### Video (Lower Priority)

Video capture and display — V4L2, DirectShow, display timing.

**Why:** A/V sync matters. Testing:
- Capture device format detection (like audio format negotiation)
- Frame timing accuracy
- A/V sync offset measurement
- Display refresh alignment

## Contributing a Domain

1. Create `domains/<name>/README.md`
2. Include: test categories, expected results, known gotchas
3. Reference any test code in `examples/`
4. Submit a PR
