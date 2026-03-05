# Optimizer Agent

Analyze a running audio configuration and identify latency bottlenecks, unnecessary abstraction layers, and optimization opportunities. Produces a diagnostic report with actionable fixes.

## Capabilities

- Measure round-trip latency via loopback test
- Detect format conversion overhead (is `plughw` silently converting?)
- Identify software mixing layers that aren't needed
- Check USB interrupt scheduling and IRQ affinity
- Verify RT thread priorities are correctly applied
- Detect CPU frequency scaling that hurts audio performance
- Check for USB bandwidth contention across shared hubs
- Compare current config against the optimal config from config-generator

## Diagnostic Commands

```bash
# Check what ALSA is actually doing (format conversion?)
cat /proc/asound/card*/pcm*/sub*/hw_params

# Check USB interrupt intervals
cat /proc/interrupts | grep -i usb

# Check RT priorities of running audio threads
ps -eo pid,cls,rtprio,ni,comm | grep -E "jackd|pipewire|waveloop"

# Check CPU governor (performance vs powersave)
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# USB device power management state
cat /sys/bus/usb/devices/*/power/control

# ALSA buffer underruns (xruns)
cat /proc/asound/card*/pcm*/sub*/status
```

## Output

A diagnostic report:

```
Signal-Chain Optimizer Report
=============================

Hardware: Volt 276 (hw:2,0) + MiniLab 3 (hw:4,0,0)
Platform: Linux aarch64, kernel 6.12.47

[PASS] Capture format: S32_LE native (no conversion)
[PASS] Playback format: S16_LE native (no conversion)
[WARN] CPU governor: ondemand → recommend 'performance' for RT audio
[WARN] USB autosuspend enabled for Volt 276 → may cause dropouts
[PASS] RT priority: audio thread at rtprio 95
[PASS] No software mixing active (dmix bypassed)
[FAIL] IRQ affinity: USB host controller sharing CPU 0 with system tasks
       Fix: echo 2 > /proc/irq/42/smp_affinity (pin to CPU 1)

Estimated round-trip latency: 2.7ms (64 samples @ 48kHz, 2 periods)
Theoretical minimum: 2.7ms — you're at the floor.

Optimization score: 87/100
```

## Usage

```
/signal-chain:optimize
```

Run on the target machine with the audio stack active. Compares actual behavior against theoretical optimal.
