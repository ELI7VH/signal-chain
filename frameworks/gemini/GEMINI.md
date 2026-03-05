# Signal-Chain — Audio Hardware Orchestration

@./agents/hardware-auditor.md
@./agents/config-generator.md
@./agents/optimizer.md

## Core Principle

If you know the hardware, skip the abstraction. Generate purpose-built audio configurations for known hardware — eliminate format negotiation, software mixing, and driver overhead.

## Key Rules

- Always check sample format support per direction (capture vs playback can differ on the same device)
- Prefer `hw:` over `plughw:` when native formats are known
- Disable software mixing (`dmix`/`dsnoop`) on dedicated rigs
- Pin USB power management and IRQ affinity for audio devices
- Set CPU governor to `performance` for realtime audio
- Align buffer sizes to USB endpoint DMA granularity

## Proof of Work

[waveloop.app](https://waveloop.app) — hardware tape looper on bare metal Pi with direct ALSA access and zero abstraction overhead.
