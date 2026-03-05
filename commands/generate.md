---
name: generate
description: Generate optimal audio configuration from a hardware fingerprint
---

Run the config-generator agent to produce platform-specific audio configuration artifacts from a `hardware-fingerprint.json`.

If no fingerprint exists, run `/signal-chain:audit` first.

## Steps

1. Read `hardware-fingerprint.json` from the current directory
2. Ask for rig intent (recording, live performance, looping, synthesis, DJ)
3. Ask for latency target (ultra-low, low, balanced, relaxed)
4. Generate all config artifacts into `./signal-chain-output/`:
   - Linux: `.asoundrc`, udev rules, RT limits, PipeWire/JACK config, systemd dependencies
   - macOS: aggregate device config, CoreAudio overrides
   - Windows: ASIO shim config, WASAPI exclusive mode setup
5. Print installation instructions for each generated file
6. Explain what abstraction layers were eliminated and why
