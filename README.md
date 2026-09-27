# XtremeASIO

Minimal Windows x64 ASIO drivers for a Behringer UMC204HD plus JBL Xtreme 4 aggregate device and an independent JBL-only direct-KS device. The JBL USB audio function is exposed as `Zgmicro AUDIO`.

The Behringer native ASIO driver is the 48 kHz master clock. XtremeASIO exposes its two inputs and four outputs, adds a stereo JBL output pair, and sends that pair directly to the physical Zgmicro Kernel Streaming (KS) render pin. WASAPI Exclusive remains available only in the standalone diagnostic backend.

## Driver choices

- **XtremeASIO UMC + JBL** (`XtremeASIO.dll`): 2 UMC inputs, 4 UMC outputs, and 2 JBL outputs with selectable 64/128/256/512-sample host buffers.
- **XtremeASIO JBL Only** (`XtremeASIO_JBL.dll`): zero inputs and one stereo JBL output using the previously proven fixed 96-frame, 48 kHz direct-KS path. It does not open the Behringer driver.

Both drivers read `jbl_gain_db` from the same per-user YAML. JBL-only intentionally requires `sample_rate: 48000`; the aggregate driver additionally supports a 44.1 kHz host through nominal conversion to the JBL's native 48 kHz stream.
## Current validated result

- ASIO interface: 48 kHz with selectable 64, 128, 256, or 512-sample host buffers; 64 is preferred by default.
- Inputs 1-2: UMC204HD inputs 1-2.
- Outputs 1-4: UMC204HD outputs 1-4 at unity gain.
- Outputs 5-6: JBL left/right through direct KS and the configured JBL-only gain.
- UMC native ASIO format: signed 32-bit little-endian integer.
- JBL KS format: 48 kHz stereo PCM16, two queued 96-frame packets.

| ASIO buffer | Input latency | Aggregate output latency | Reported round trip |
|---:|---:|---:|---:|
| 64 | 136 frames / 2.83 ms | 272 frames / 5.67 ms | 408 frames / 8.50 ms |
| 128 | 200 frames / 4.17 ms | 336 frames / 7.00 ms | 536 frames / 11.17 ms |
| 256 | 328 frames / 6.83 ms | 464 frames / 9.67 ms | 792 frames / 16.50 ms |
| 512 | 584 frames / 12.17 ms | 720 frames / 15.00 ms | 1304 frames / 27.17 ms |

All four modes were validated against the connected hardware. The 128, 256, and 512 modes completed their eight-second runs with zero concealment, transport errors, or late gaps. The 64 mode remains the low-latency playing profile and uses last-pair concealment plus a short recovery crossfade for rare small bridge shortfalls.

The latency figures are driver/transport reports, not acoustic loopback measurements. USB converters, JBL DSP, amplification, and air-path delay can add latency; an electrical/acoustic loopback measurement is still required.

## Build

Use a current x64 Visual Studio/MSVC installation and CMake:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Release artifacts:

- `build\Release\XtremeASIO.dll` - 64-bit aggregate ASIO COM driver
- `build\Release\XtremeASIO_JBL.dll` - independent 64-bit JBL-only ASIO COM driver
- `build\Release\xtremeasio_diag.exe` - JBL endpoint, gain, WASAPI probe, and telemetry utility
- `build\Release\asio_smoke.exe` - direct aggregate-ASIO smoke host
- `build\Release\asio_probe.exe` - native ASIO capability probe
- `build\Release\ks_test.exe` - direct JBL KS diagnostic
- `build\Release\wasapi_test.exe` - standalone WASAPI Exclusive diagnostic

## Configure and test

Driver settings are stored in `%LOCALAPPDATA%\XtremeASIO\config.yaml`:

```yaml
sample_rate: 48000
preferred_buffer_size: 64
jbl_gain_db: -18.00
```

Supported sample rates are 44100 and 48000 Hz; 48000 is recommended and is the maximum supported configuration. At 44.1 kHz the UMC and host run at 44.1 kHz while the preallocated bridge converts the JBL pair to its native 48 kHz stream. Supported preferred buffer sizes are 64, 128, 256, and 512. Gain accepts -96 through 0 dB and affects only JBL outputs 5/6. Close and reopen the ASIO device after editing.

Print the active path and parsed values with:

```powershell
build\Release\xtremeasio_diag.exe config
```

The UMC204HD driver must be installed and both USB devices must be connected before the driver is opened.

```powershell
build\Release\xtremeasio_diag.exe select
build\Release\xtremeasio_diag.exe gain -18
build\Release\xtremeasio_diag.exe xruns
build\Release\asio_smoke.exe build\Release\XtremeASIO.dll
```

JBL gain is stored per user under `HKCU\Software\XtremeASIO`, accepts `-96` through `0` dB, and defaults to `-18 dB`. It is read when the driver loads. UMC outputs remain at unity gain.

An optional aggregate-path JBL tone test accepts the desired post-gain dBFS level and duration:

```powershell
build\Release\asio_smoke.exe build\Release\XtremeASIO.dll tone -36 10 64
```

## Register and use in Ableton Live

Close Ableton first. In an **Administrator PowerShell** from this directory:

```powershell
& "$env:WINDIR\System32\regsvr32.exe" "$(Resolve-Path build\Release\XtremeASIO.dll)"
& "$env:WINDIR\System32\regsvr32.exe" "$(Resolve-Path build\Release\XtremeASIO_JBL.dll)"
```

Then in Ableton Live:

1. Open **Options > Preferences > Audio**.
2. Set **Driver Type** to **ASIO**.
3. Select **XtremeASIO UMC + JBL** for the aggregate interface or **XtremeASIO JBL Only** when the Behringer is not needed. If an older label remains visible, re-register both DLLs as Administrator and restart Live.
4. Use **48000 Hz**, then choose **64**, **128**, **256**, or **512** samples. Use 64 for live playing and 128-512 for progressively heavier mixing sessions.
5. Open **Input Config** and enable mono inputs 1 and 2 (and stereo 1/2 if desired).
6. Open **Output Config** and enable the pairs you need.
7. Route channels using this map:

| ASIO channel | Ableton label | Physical route |
|---|---|---|
| Input 1 | UMC In 1 | UMC204HD input 1 |
| Input 2 | UMC In 2 | UMC204HD input 2 |
| Output 1 | UMC Out 1 | UMC204HD output 1 |
| Output 2 | UMC Out 2 | UMC204HD output 2 |
| Output 3 | UMC Out 3 | UMC204HD output 3 |
| Output 4 | UMC Out 4 | UMC204HD output 4 |
| Output 5 | JBL Out L | JBL left |
| Output 6 | JBL Out R | JBL right |

For the JBL master path, choose Ext. Out 5/6 on Live's Master track. For the Behringer main pair, choose 1/2.

The JBL KS pin is exclusive. Close other applications using Zgmicro and route normal Windows audio elsewhere. The Behringer native ASIO driver is also opened exclusively by XtremeASIO, so do not select the Behringer ASIO device in another application at the same time.

To unregister:

```powershell
& "$env:WINDIR\System32\regsvr32.exe" /u "$(Resolve-Path build\Release\XtremeASIO_JBL.dll)"
& "$env:WINDIR\System32\regsvr32.exe" /u "$(Resolve-Path build\Release\XtremeASIO.dll)"
```

## Scope and licensing

This milestone provides the requested 2-in/6-out aggregate topology and selectable 64/128/256/512-sample ASIO buffers. It does not yet provide multi-device input or a polished control panel. The cross-device JBL bridge uses a bounded linear drift resampler because the UMC and JBL have independent USB clocks.

The repository vendors the ASIO 2.3 interface SDK in `third_party/asio-sdk`. Steinberg offers it under a dual proprietary/GPLv3 license. Review `third_party/asio-sdk/LICENSE.txt` before redistributing binaries; this project currently assumes the GPLv3 option unless you obtain the proprietary agreement.