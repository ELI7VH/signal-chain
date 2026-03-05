# Config Generator Agent

Consume a hardware fingerprint and generate platform-specific audio configuration artifacts optimized for the exact hardware present. No abstraction layers. No format negotiation. No guessing.

## Inputs

- `hardware-fingerprint.json` from the hardware-auditor agent
- User intent: what the rig is for (recording, live performance, looping, synthesis, DJ, etc.)
- Latency target: ultra-low (<3ms), low (<10ms), balanced (<20ms), relaxed (don't care)

## Generated Artifacts

### Linux

**`.asoundrc`** — Direct `hw:` device mapping with native formats:
```
pcm.volt276_capture {
    type hw
    card 2
    device 0
    format S32_LE
    rate 48000
    channels 2
}

pcm.volt276_playback {
    type hw
    card 2
    device 0
    format S16_LE
    rate 48000
    channels 2
}
```

No `plughw`. No `dmix`. No `dsnoop`. Direct DMA.

**`udev` rules** — Pin USB power management and identify devices by VID/PID:
```
# Volt 276: disable autosuspend, set USB scheduler
ACTION=="add", ATTR{idVendor}=="2f15", ATTR{idProduct}=="0101", \
    ATTR{power/autosuspend}="-1", \
    ATTR{power/control}="on"

# Set IRQ affinity for audio USB host controller
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="2f15", \
    RUN+="/usr/local/bin/signal-chain-irq-pin.sh %k"
```

**RT tuning** — Thread priority and CPU affinity:
```ini
# /etc/security/limits.d/signal-chain.conf
@audio - rtprio 95
@audio - memlock unlimited
@audio - nice -20
```

**PipeWire/JACK graph** — If PipeWire or JACK is in use, generate the optimal routing:
```json
{
  "context.objects": [
    {
      "factory": "adapter",
      "args": {
        "node.name": "volt276",
        "media.class": "Audio/Duplex",
        "api.alsa.path": "hw:2,0",
        "audio.format": "S32",
        "audio.rate": 48000,
        "audio.channels": 2,
        "api.alsa.period-size": 64,
        "api.alsa.headroom": 0
      }
    }
  ]
}
```

**Systemd service dependencies** — Ensure USB devices are enumerated before the audio service starts:
```ini
[Unit]
After=sys-devices-platform-soc-fe980000.usb-usb1-1\x2d1-1\x2d1.4.device
BindsTo=sys-devices-platform-soc-fe980000.usb-usb1-1\x2d1-1\x2d1.4.device
```

### macOS

**Aggregate device configuration** — For multi-device setups
**CoreAudio HAL overrides** — Buffer size, sample rate lock

### Windows

**Custom ASIO shim** — Bypass ASIO4ALL, map directly to device endpoints
**WASAPI exclusive mode configuration** — Pin format and buffer size

## Buffer Size Calculation

The optimal buffer size is derived from:
1. USB endpoint max packet size and polling interval
2. Host USB controller interrupt coalescing
3. User's latency target
4. Device's minimum period size

Formula: `optimal_period = max(device_min_period, ceil(target_latency_samples / n_periods))`

Where `n_periods` is typically 2 (double-buffered) or 3 (triple for safety).

## Usage

```
/signal-chain:generate
```

Reads the fingerprint, asks for intent and latency target, outputs all config files to `./signal-chain-output/`.

## Anti-Patterns This Eliminates

- `plughw` format conversion on every buffer cycle
- Software mixing (`dmix`) when only one application uses the device
- Sample rate conversion when device and application agree on rate
- Channel mapping overhead for stereo-to-stereo paths
- PulseAudio/PipeWire resampling when direct access is possible
