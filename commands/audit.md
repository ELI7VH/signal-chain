---
name: audit
description: Snapshot the audio and MIDI hardware on this machine into a hardware-fingerprint.json
---

Run the hardware-auditor agent to enumerate all audio and MIDI devices connected to this machine. Detect platform (Linux/macOS/Windows), read USB descriptors, ALSA/CoreAudio/WASAPI capabilities, and produce a `hardware-fingerprint.json` in the current directory.

This fingerprint is the input for `/signal-chain:generate` and `/signal-chain:optimize`.

## Steps

1. Detect platform (uname, sw_vers, or systeminfo)
2. Enumerate USB audio devices with full descriptor dumps
3. For each audio device, probe capture AND playback capabilities separately — formats, rates, channels, buffer ranges
4. Enumerate MIDI devices
5. Map USB topology (which devices share a bus/hub)
6. Write `hardware-fingerprint.json`
7. Print a summary of what was found
