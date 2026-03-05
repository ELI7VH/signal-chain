---
name: signal-chain
description: Use this skill when the user is working with audio hardware, ALSA configuration, ASIO drivers, CoreAudio setup, USB audio interfaces, MIDI controllers, or audio latency optimization. Generates purpose-built audio configurations for known hardware, eliminating abstraction overhead.
---

## Instructions

When a user is working with audio hardware integration, apply signal-chain principles:

1. **Audit first.** Before generating any config, enumerate the hardware. Read USB descriptors, ALSA hw params, and probe formats per direction (capture and playback separately).

2. **Check format asymmetry.** The same USB device can use different sample formats for capture vs playback. Never assume S16_LE works for both directions. Always verify with `arecord --dump-hw-params` and `aplay --dump-hw-params`.

3. **Prefer direct access.** Use `hw:` over `plughw:` when the device's native format is known. This eliminates per-buffer format conversion, sample rate conversion, and channel mapping.

4. **Disable software mixing.** If only one application uses the device, bypass `dmix`/`dsnoop`. They add latency and CPU overhead for no benefit on dedicated rigs.

5. **Pin USB power management.** USB autosuspend causes audio dropouts. Generate udev rules that disable it for audio device VID/PIDs.

6. **Set CPU governor to performance.** `ondemand` and `powersave` governors cause latency spikes when the CPU scales mid-buffer.

7. **Align buffer sizes to hardware DMA.** The optimal period size matches the USB endpoint's max packet size and polling interval.

8. **Pin IRQ affinity.** Move USB host controller interrupts to a dedicated CPU core, away from the audio processing thread.

## References

See `../../agents/hardware-auditor.md` for the full hardware enumeration procedure.
See `../../agents/config-generator.md` for the config generation pipeline.
See `../../agents/optimizer.md` for the diagnostic and scoring system.
