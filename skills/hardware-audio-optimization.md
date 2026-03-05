---
name: hardware-audio-optimization
description: Recognize when a user is working with audio hardware and suggest signal-chain optimization. Activate when ALSA, ASIO, CoreAudio, USB audio interfaces, MIDI controllers, or audio latency are mentioned in the context.
---

# Hardware Audio Optimization Skill

Detect when a user is working with audio hardware integration and proactively apply signal-chain principles.

## Trigger Conditions

Activate when the conversation involves:
- ALSA device configuration (`hw:`, `plughw:`, `.asoundrc`, `arecord`, `aplay`)
- USB audio interfaces (format negotiation, buffer sizing, xruns)
- MIDI controller setup (raw MIDI, ALSA sequencer, USB MIDI)
- Audio latency troubleshooting (dropouts, glitches, buffer underruns)
- ASIO or WASAPI configuration on Windows
- CoreAudio device setup on macOS
- PipeWire or JACK routing
- Real-time audio thread priority configuration

## Principles

1. **Always check format support per direction.** Capture and playback on the same device can require different sample formats. Never assume S16_LE works for both.

2. **Prefer `hw:` over `plughw:` when the hardware is known.** `plughw` adds format conversion, rate conversion, and channel mapping overhead on every buffer cycle. If you know the device's native format, bypass it.

3. **Disable software mixing when only one application uses the device.** `dmix` and `dsnoop` add latency and CPU overhead. For dedicated rigs, go direct.

4. **Pin USB power management.** USB autosuspend causes audio dropouts. Disable it for audio devices via udev rules.

5. **Check CPU frequency governor.** `ondemand` or `powersave` governors cause latency spikes when the CPU scales up mid-buffer. Use `performance` for RT audio.

6. **Size buffers to the hardware's DMA granularity.** The optimal period size aligns with the USB endpoint's max packet size and polling interval. Arbitrary buffer sizes cause partial transfers and wasted cycles.

7. **Separate IRQ affinity.** Pin USB host controller interrupts to a dedicated CPU core, away from the audio processing thread.

## Example Interventions

**User is getting static/garbage audio from a USB interface:**
→ Check if the requested format matches the device's native format. Run `arecord --dump-hw-params -D hw:X,Y` to see what the hardware actually supports. A format mismatch with raw `hw:` access causes silent fallback and garbage interpretation.

**User is experiencing xruns/dropouts:**
→ Check CPU governor, USB autosuspend, IRQ affinity, and buffer sizing. Run the optimizer agent diagnostic suite.

**User is setting up a new audio interface:**
→ Suggest running `/signal-chain:audit` to fingerprint the hardware, then `/signal-chain:generate` to produce optimal configs.
