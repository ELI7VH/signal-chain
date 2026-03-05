# Signal-Chain — Audio Hardware Orchestration

This project provides AI-generated custom audio configurations for known hardware. Instead of generic driver abstraction layers, signal-chain reads the exact hardware on a machine and produces purpose-built configs that eliminate unnecessary overhead.

## Core Principle

If you know the hardware, skip the abstraction. The abstraction tax is real — format negotiation, software mixing, sample rate conversion, channel mapping — all unnecessary when the hardware is fixed and known.

## Agents

See `agents/` for specialist agent definitions:
- **hardware-auditor** — enumerate and fingerprint audio/MIDI hardware
- **config-generator** — generate platform-specific optimal configs from a fingerprint
- **optimizer** — diagnose a running audio stack, score latency, find bottlenecks

## Workflow

1. Run the hardware auditor to produce `hardware-fingerprint.json`
2. Run the config generator to produce optimal configs in `signal-chain-output/`
3. Optionally run the optimizer to diagnose an active audio stack

## Key Technical Context

- USB audio devices can use different sample formats per direction (capture vs playback)
- `plughw` adds format conversion overhead on every buffer cycle — prefer `hw:` when formats are known
- USB autosuspend causes audio dropouts — disable via udev rules
- CPU frequency governors (`ondemand`/`powersave`) cause latency spikes — use `performance` for RT audio
- IRQ affinity for USB host controllers should be pinned away from audio processing cores

## Proof of Work

[waveloop.app](https://waveloop.app) — hardware tape looper on Raspberry Pi 4 with direct ALSA `hw:` access, per-direction format negotiation, and zero abstraction overhead.
