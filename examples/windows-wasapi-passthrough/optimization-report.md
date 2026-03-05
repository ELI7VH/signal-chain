# Signal-Chain Optimization Report
**Machine:** DESKTOP-NLGSHV0 (ASUS X510UAR laptop)
**Date:** 2026-03-04

## Hardware Summary

| Component | Detail |
|-----------|--------|
| CPU | Intel i7-8550U — 4C/8T @ 1.8GHz (turbo 2.0GHz) |
| RAM | 16 GB DDR4-2400 (Kingston + Samsung mixed) |
| GPU | Intel UHD 620 (integrated) |
| Boot drive | Samsung 860 EVO M.2 500GB (SATA) |
| Second drive | Micron 1100 2TB SATA SSD (RAW — reserved for Ubuntu) |
| Audio | Conexant SmartAudio HD (built-in laptop) |
| USB audio | **NONE connected** |
| MIDI | teVirtualMIDI + Bome Virtual MIDI (software only) |
| Bluetooth audio | Sony MDR-XB950B1, DPX50* |

---

## Optimization Checklist

### Already Applied (debloat 2026-03-04)

| # | Check | Status |
|---|-------|--------|
| 1 | Power plan: High Performance | **PASS** |
| 2 | USB selective suspend: Disabled | **PASS** |
| 3 | Processor scheduling: Programs | **PASS** |
| 4 | System sounds: No Sounds | **PASS** |
| 5 | Game DVR / Game Bar: Disabled | **PASS** |
| 6 | Search Indexing: Disabled | **PASS** |
| 7 | Cortana: Disabled | **PASS** |
| 8 | Delivery Optimization P2P: Disabled | **PASS** |
| 9 | Startup delay: 0ms | **PASS** |
| 10 | Visual animations: Off | **PASS** |

### Remaining Optimizations

| # | Issue | Severity | Recommendation |
|---|-------|----------|----------------|
| 1 | **No USB audio interface connected** | INFO | Built-in Conexant is adequate for casual use. For real production, plug in a dedicated USB interface (Volt 276, Scarlett, etc.) and re-run audit. |
| 2 | **ASIO4ALL is a wrapper, not native ASIO** | WARN | ASIO4ALL adds overhead. When a USB interface is connected, use its native ASIO driver instead. |
| 3 | **Bluetooth audio adds 100-300ms latency** | WARN | Sony MDR-XB950B1 and DPX50* should NOT be used for monitoring during recording/production. Fine for casual listening. |
| 4 | **Hyper-V virtual switches active** | WARN | WSL's Hyper-V adapters can cause occasional DPC latency spikes. If you experience audio dropouts, disable WSL temporarily or test with LatencyMon. |
| 5 | **Intel Wi-Fi AC 8265 — DPC risk** | WARN | Intel WiFi drivers are a known source of DPC latency on laptops. If you get xruns during recording, temporarily disable Wi-Fi. |
| 6 | **Virtual Desktop Audio still installed** | INFO | Orphaned software audio device. Can be removed if Virtual Desktop is no longer used. |
| 7 | **FL Studio ASIO driver registered** | INFO | Only usable inside FL Studio. Not harmful, but could confuse other DAWs that enumerate ASIO drivers. |
| 8 | **Conexant services running** | INFO | CxAudMsg + SAService are Conexant helper services. Required for laptop speakers. No action needed. |
| 9 | **BTAGService running** | INFO | Bluetooth Audio Gateway — only needed for BT headset calls. Can be set to Manual if not using BT for calls. |
| 10 | **GPU: integrated Intel UHD 620** | INFO | Not a bottleneck for audio. Could limit game dev with heavy rendering. |

---

## DPC Latency Risk Assessment

| Source | Risk | Notes |
|--------|------|-------|
| Intel WiFi AC 8265 | **Medium** | Known DPC offender. Disable during recording sessions if needed. |
| Hyper-V (WSL) | **Low-Medium** | Virtual switches add occasional interrupt overhead. |
| Bluetooth | **Low** | Not a DPC issue, but adds transport latency to audio. |
| Conexant HDA | **Low** | Typical laptop HDA codec. |
| Samsung 860 EVO | **Negligible** | SATA SSDs are well-behaved. |

**Recommendation:** Run [LatencyMon](https://www.resplendence.com/latencymon) for 10 minutes during a typical workload to baseline actual DPC behavior.

---

## Audio Path Analysis

### Current Path (Built-in speakers)
```
Application → WASAPI → Windows Audio Service → Conexant HDA driver → Speakers
                        ↑ mixing layer ↑        ↑ format conversion possible ↑
```
**Latency:** ~10-30ms depending on buffer size

### With ASIO4ALL
```
Application → ASIO4ALL → WDM kernel streaming → Conexant HDA driver → Speakers
              ↑ wrapper overhead ↑
```
**Latency:** ~5-15ms (better than WASAPI shared, worse than native ASIO)

### Optimal Path (with USB interface + native ASIO)
```
Application → Native ASIO driver → USB audio hardware
              ↑ zero overhead ↑     ↑ direct DMA ↑
```
**Latency:** ~2-5ms (achievable with Volt 276 at 64 samples/48kHz)

---

## Recommendations by Use Case

### Music Production (primary)
1. **Plug in a USB audio interface** — the Volt 276 you use on the Pi would work great here too
2. Use its **native ASIO driver** (not ASIO4ALL)
3. Disable WiFi during recording sessions if you hear crackles
4. Monitor through the interface, not Bluetooth headphones

### Casual Game Dev
- Current setup is fine
- Intel UHD 620 handles 2D and lightweight 3D
- No audio optimization needed for game testing

### Docker Services
- Current setup is fine
- The 2TB Micron SSD (once Ubuntu is installed) will be the better host for Docker workloads
- WSL Docker is adequate for development

### Music Tool Development
- teVirtualMIDI + Bome Virtual MIDI provide good inter-app MIDI routing
- When testing MIDI hardware (Arturia MiniLab etc.), connect via USB and re-audit
- ASIO4ALL can be used for testing WASAPI/ASIO paths without dedicated hardware

---

## Optimization Score

| Category | Score | Max |
|----------|-------|-----|
| Power management | 20 | 20 |
| OS debloat | 20 | 20 |
| Audio hardware | 5 | 25 |
| ASIO/driver stack | 5 | 15 |
| DPC latency risk | 12 | 15 |
| MIDI routing | 5 | 5 |
| **Total** | **67** | **100** |

**67/100 — Good OS foundation, limited by no dedicated audio hardware connected.**

The debloat and power tuning are optimal. The score jumps to ~90+ once a USB audio interface with native ASIO is connected.

---

*Generated by signal-chain hardware auditor agent*
*Fingerprint: `hardware-fingerprint.json`*
