# signal-chain

**AI-generated custom audio orchestration for known hardware configurations.**

Your machine has a fixed set of hardware. Your audio stack doesn't know that. Signal-chain bridges the gap: snapshot your hardware, generate the lowest-latency integration possible, skip every abstraction layer that exists to handle devices you'll never plug in.

## The Problem

Every audio stack is built for the general case:

- **ALSA** `plughw` negotiates format, sample rate, channel count, and software mixing on every buffer cycle — because it doesn't know what's on the other end
- **ASIO** exists because the Windows audio path (WDM → KMixer → WASAPI) adds 3-4 layers of indirection that nobody doing realtime audio wants
- **CoreAudio** is better but still generalized — AUGraph routing, format conversion, aggregate device abstraction

The abstraction tax is real. Generic drivers handle every possible device, which means format negotiation overhead, buffer size guessing, routing through mixing layers that aren't needed, and interrupt coalescing tuned for "average" use cases.

**If you know the exact hardware, you can skip all of it.**

## The Idea

An AI agent that:

1. Reads your hardware snapshot — `lsusb -v` descriptors, `arecord --dump-hw-params`, ALSA capabilities, USB endpoint configurations
2. Generates a purpose-built audio orchestration layer — not a kernel driver, but the optimal configuration between the kernel and your application
3. Outputs concrete artifacts: `.asoundrc` profiles, ALSA UCM configs, JACK/PipeWire routing graphs, udev rules, RT thread priority tuning, CPU core pinning, ASIO shims

Hard-coded format paths. Exact buffer geometry for your device's DMA transfer size. Zero software mixing. Direct `hw:` access. USB power management and IRQ affinity pinned per VID/PID.

## How We Got Here

This idea didn't come from a whiteboard. It fell out of a week of building a hardware tape looper on a Raspberry Pi.

### The Origin

In February 2026, [Elijah Lucian](https://elijahlucian.ca) borrowed a friend's OP-1 Field. Used it for two days, had to give it back, got FOMO, and decided to build his own tape looper. The first version was a [JSFX plugin](https://waveloop.app) in REAPER — a single-lane tape engine with destructive overdub, ratio speeds, and an in/out/loop model. Then a 4-track version. Then a native macOS app in Swift with CoreAudio. Then an iOS AUv3. Then, on March 2nd, he decided to go embedded.

### The Pi Build

Raspberry Pi 4. HyperPixel 4.0 DPI display (480x800 portrait). Universal Audio Volt 276 USB interface. Arturia MiniLab 3 MIDI controller. Bare metal C — ALSA for audio, raw MIDI via `snd_rawmidi`, SDL2 for display, lock-free pthreads for the audio engine.

The first three days were hardware wrestling. WiFi soft-blocked by rfkill on every boot. UK keyboard layout by default. Display driver conflicts causing boot hangs. Root shell recovery. Cloud-init refusing to re-run. The HyperPixel uses DPI GPIO pins that conflict with DSI auto-detection — `display_auto_detect=1` had to be forced to `0`.

> "she built baby!"

The binary compiled. No display. No audio. No MIDI. Just a log full of `No such file or directory`.

### The Format Discovery

Then the Volt 276 was plugged in. The audio engine connected. And produced **loud static**.

The diagnosis: line 85 of `audio.c` called `snd_pcm_hw_params_set_format(SND_PCM_FORMAT_S16_LE)` without checking the return value. The Volt 276 natively uses **S32_LE for capture** but **S16_LE for playback**. With raw `hw:` access (no plugin conversion layer), the format set failed silently, ALSA defaulted to S32_LE, and the code interpreted 32-bit samples as 16-bit — 2x heap overflow on every buffer read, complete garbage audio.

**The same device. Two different native formats. Depending on direction.**

The fix: auto-negotiate format per direction. Try S16, fall back to S32, fall back to S24. Convert in the audio thread based on what the hardware actually accepted. Direct `hw:` access with known formats, zero `plughw` overhead.

> "fuckin works bud!"

### The Realization

That fix was the proof of concept for signal-chain. By *knowing* the hardware — Volt 276 capture is S32_LE, playback is S16_LE — we bypassed every abstraction layer. No format negotiation per buffer. No `plughw` conversion. No software mixing. Direct DMA-aligned transfers in the device's native format.

The latency improvement was measurable. And the configuration was trivial — *if you know what you're plugging in*.

The gap: nobody wants to manually audit USB descriptors, read ALSA capability dumps, and hand-write `.asoundrc` files for their specific rig. But an AI can do exactly that.

> "Like, with AI, if AI knows the exact hardware that a unit will use, doesn't it stand possible to create an orchestration pattern that will tell the AI to create a custom hardware flow for a combo of tech."
>
> "basically: 'hey AI I have my machine set up in the exact hardware configuration it will be. Create the lowest level of integrations.'"

### What Happened Next

14 DSP effects (delay, chorus, phaser, flanger, grit, bitcrush, radio artifacts, filters). A config panel on the hardware encoder. MIDI pad mapping with SysEx LED feedback. A JSFX mirror/ref system ported from the REAPER plugin. The display aligned pixel-for-pixel with the macOS app. A waveOS disk image published to a CDN.

> "OMGGGG this is SO fucking COOL"
>
> "this is the coolest shit ever."

Then the SD card corrupted from a hard unplug. While setting up WSL on another laptop to attempt disk repair at 4am, the signal-chain idea dropped.

## Proof of Work

**[waveloop.app](https://waveloop.app)** — the tape looper that spawned this idea. Hardware tape engine running on bare metal Pi with direct ALSA `hw:` access, per-direction format negotiation, and zero abstraction overhead. The entire journey is documented on the site timeline.

## What Signal-Chain Generates

For a given hardware snapshot, signal-chain would produce:

### Linux (ALSA / JACK / PipeWire)
- Custom `.asoundrc` with hard-coded `hw:` device paths and native formats
- ALSA UCM (Use Case Manager) profiles for the exact device topology
- JACK or PipeWire routing graphs with optimal buffer sizes
- `udev` rules: USB autosuspend disabled, IRQ affinity pinned per VID/PID
- RT thread priority assignments and CPU core pinning
- Systemd service files with correct `After=` dependencies for USB enumeration order

### Windows (ASIO / WASAPI)
- Purpose-built ASIO shim that bypasses ASIO4ALL's generic device enumeration
- Direct USB endpoint mapping for known device descriptors
- Buffer size tuned to the device's actual DMA transfer granularity
- IRQ affinity and thread priority for the audio processing core

### macOS (CoreAudio)
- Aggregate device configurations for multi-device setups
- `AudioServerPlugIn` configurations optimized for known hardware
- IOProc thread priority and scheduling hints

### Cross-Platform
- Hardware capability report (formats, rates, channels, latencies per direction)
- Optimal buffer size calculation based on device DMA and host interrupt intervals
- Format conversion elimination map (which paths need no conversion)

## Framework Support

Signal-chain is framework-agnostic. The agent definitions are plain markdown — they describe *what to do*, not how any specific tool does it. Framework adapters provide the manifest wiring for each platform.

```
frameworks/
  claude/     — .claude-plugin/plugin.json, symlinks to shared agents/commands/skills
  codex/      — AGENTS.md + .codex/skills/signal-chain/SKILL.md
  gemini/     — GEMINI.md + .gemini/commands/*.toml (with shell + file injection)
  custom/     — Guide for adapting to any framework
```

| Framework | Install | Commands |
|-----------|---------|----------|
| **Claude Code** | Copy `.claude-plugin/`, symlink agents/commands/skills | `/signal-chain:audit`, `/signal-chain:generate` |
| **Codex CLI** | Copy `AGENTS.md` + `.codex/skills/` | Skill auto-activates on audio topics |
| **Gemini CLI** | Copy `GEMINI.md` + `.gemini/commands/` | `/audit`, `/generate` |
| **Cursor** | Concatenate agents into `.cursor/rules/` | Agent-decided activation |
| **Other** | Concatenate agent markdown into your instructions file | Ask directly |

See each framework's `README.md` for setup details.

### Adding a New Framework

1. Create `frameworks/<name>/`
2. Add the platform's manifest/config that references the shared agent files
3. Add a `README.md` with setup instructions
4. Submit a PR

The agent content stays in one place. Framework adapters are just wiring.

## Status

This is a concept + proof of work, not a finished tool. The WaveLoop Pi build proved the principle. The next step is building the snapshot → config generator.

Contributions, ideas, and war stories about audio driver pain are welcome.

## Related

- [waveloop.app](https://waveloop.app) — the tape looper that started it all
- [WaveLoop Pi source](https://github.com/ELI7VH/lucian-utils/tree/main/raspberry-pi/waveloop-x1) — the embedded C implementation
- [ALSA UCM docs](https://www.alsa-project.org/alsa-doc/alsa-lib/ucm.html) — Use Case Manager reference
- [PipeWire filter chains](https://docs.pipewire.org/page_man_pipewire-filter-chain_7.html) — PipeWire's programmable DSP routing

## License

MIT
