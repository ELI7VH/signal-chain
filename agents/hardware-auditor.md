# Hardware Auditor Agent

Enumerate and analyze the audio and MIDI hardware connected to a machine. Produce a structured hardware fingerprint that downstream agents use to generate optimized configurations.

## Capabilities

- Read USB device descriptors via `lsusb -v` and parse VID/PID, device class, endpoint configurations, max packet sizes, and polling intervals
- Enumerate ALSA devices: `arecord -l`, `aplay -l`, `arecord --dump-hw-params`, `aplay --dump-hw-params`
- Detect native sample formats per direction (capture vs playback may differ — e.g., Volt 276 uses S32_LE capture, S16_LE playback)
- Enumerate MIDI devices via `amidi -l` or `/proc/asound/*/midi*`
- Read PipeWire/JACK device graphs if running
- On macOS: parse `system_profiler SPUSBDataType` and CoreAudio device properties
- On Windows: enumerate WASAPI devices and ASIO driver registry entries

## Output Format

Produce a `hardware-fingerprint.json`:

```json
{
  "platform": "linux-aarch64",
  "kernel": "6.12.47+rpt-rpi-v8",
  "audio_devices": [
    {
      "name": "Universal Audio Volt 276",
      "alsa_id": "hw:2,0",
      "usb_vid_pid": "2f15:0101",
      "capture": {
        "formats": ["S32_LE"],
        "rates": [44100, 48000, 88200, 96000],
        "channels": [1, 2],
        "period_size_range": [16, 65536],
        "buffer_size_range": [32, 131072]
      },
      "playback": {
        "formats": ["S16_LE", "S24_LE", "S32_LE"],
        "rates": [44100, 48000, 88200, 96000],
        "channels": [1, 2],
        "period_size_range": [16, 65536],
        "buffer_size_range": [32, 131072]
      },
      "usb_endpoints": {
        "capture": { "address": "0x86", "max_packet": 384, "interval": 1 },
        "playback": { "address": "0x05", "max_packet": 384, "interval": 1 }
      }
    }
  ],
  "midi_devices": [
    {
      "name": "Arturia MiniLab 3",
      "alsa_id": "hw:4,0,0",
      "usb_vid_pid": "1c75:02b5",
      "ports": 4,
      "port_names": ["MiniLab3 MIDI", "MiniLab3 DIN", "MiniLab3 MCU/HUI", "MiniLab3 ALV"]
    }
  ],
  "usb_topology": {
    "hub_chain": ["root_hub:1", "port:2"],
    "bus_speed": "high-speed",
    "shared_bus_devices": []
  }
}
```

## Usage

```
/signal-chain:audit
```

Run on the target machine. The fingerprint file is the input for the config-generator and optimizer agents.

## Notes

- USB device enumeration order can change across reboots — the fingerprint captures VID/PID for stable identification
- Some USB audio devices expose multiple ALSA cards (one per function) — audit all of them
- Check for USB bandwidth contention if multiple isochronous devices share a bus
- Always test both capture and playback directions separately — format support is not symmetric
