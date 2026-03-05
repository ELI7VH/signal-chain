# Signal-Chain — Claude Plugin

## What This Is

A Claude Code plugin that generates optimal audio configurations for known hardware. Instead of fighting ALSA, ASIO, or CoreAudio abstractions, signal-chain reads the exact hardware on a machine and produces purpose-built configs that eliminate every unnecessary layer.

## Plugin Structure

```
.claude-plugin/plugin.json  — Plugin metadata
agents/
  hardware-auditor.md       — Enumerate and fingerprint audio/MIDI hardware
  config-generator.md       — Generate platform-specific optimal configs
  optimizer.md              — Diagnose and score a running audio setup
commands/
  audit.md                  — /signal-chain:audit — snapshot hardware
  generate.md               — /signal-chain:generate — produce configs
skills/
  hardware-audio-optimization.md — Proactive skill for audio hardware contexts
```

## Commands

| Command | Description |
|---------|-------------|
| `/signal-chain:audit` | Fingerprint all audio and MIDI hardware on this machine |
| `/signal-chain:generate` | Generate optimal configs from the fingerprint |

## Key Principle

The abstraction tax is real. If you know the hardware, skip the abstraction.

## Proof of Work

This plugin was born from building [WaveLoop](https://waveloop.app) — a hardware tape looper on Raspberry Pi 4. The Volt 276 USB interface uses S32_LE for capture and S16_LE for playback on the same device. Generic ALSA access through `plughw` silently converts formats on every buffer cycle. By hard-coding `hw:` access with the known native formats, we eliminated format negotiation entirely and hit the theoretical minimum latency.

## Version

Bump version in `.claude-plugin/plugin.json` for any change. Follow semver:
- MINOR: new agents, commands, or skills
- PATCH: docs, fixes, improvements
