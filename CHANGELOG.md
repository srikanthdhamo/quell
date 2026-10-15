# Changelog

## 1.1.0

- **Low-latency mode is now zero latency (0 ms).** The spectral analysis runs on a side path. Its reduction curve becomes a minimum-phase FIR filter, rebuilt every ~5 ms and crossfaded, that filters the live signal. Frequency resolution matches Quality mode; reduction reacts slightly later because there is no look-ahead. Previously this mode had 10.7 ms of latency.
- **macOS support**: universal binary (Apple Silicon M1–M4 + Intel), macOS 11+, VST3 + AU + standalone app.
- Windows build links the C++ runtime statically, so the VC++ redistributable is no longer required.
- Version number shown in the plugin header.
- CI workflow builds, tests, and packages Windows and macOS. It validates the AU with `auval` and attaches zips to GitHub releases on `v*` tags.
- Build scripts: `scripts/build-windows.bat`, `scripts/build-macos.sh`.

## 1.0.0

- First version: STFT dynamic resonance suppression, EQ-style sensitivity curve, delta, mix, stereo link, low-latency mode (10.7 ms).
