# Architecture

```text
Ableton / asio_smoke (float32, 64 frames, 48 kHz)
                  |
             IASIO COM DLL
                  |
       Behringer native ASIO (master clock)
          | input 1/2          | output 1-4
          | int32 -> float     | float -> int32
          v                    v
     host callback        UMC204HD hardware
          |
     JBL output 5/6 -> JBL-only gain -> SPSC drift bridge
                                           |
                         KsRenderer (two 96-frame PCM16 packets)
                                           |
                          Zgmicro USB physical KS render pin

wasapi_test ------ WASAPI Exclusive diagnostic/reference only
ks_test ---------- direct KS transport diagnostic
xtremeasio_diag -- endpoint selection, gain, probing, shared telemetry
```

## Independent JBL-only driver

`src/asio_driver.cpp` builds as `XtremeASIO_JBL.dll` with CLSID `{A9A37F2D-6E4B-4C5B-9A1D-2E8F7B6C4D10}` and the separate ASIO registry identity `XtremeASIO JBL Only`. It exposes zero inputs and two float32 outputs at the JBL's native 48 kHz, using fixed 96-frame ASIO/KS buffers and two outstanding KS packets. It never loads or opens the Behringer ASIO driver. Its gain comes from the shared YAML file.
## Aggregate ASIO driver

`src/aggregate_asio_driver.cpp` implements `IASIO` and a small COM class factory. It loads the installed Behringer UMC ASIO DLL through its class factory, avoiding cross-apartment proxying, and accepts 44.1 or 48 kHz plus 64, 128, 256, or 512-frame power-of-two host buffers. The JBL KS pin remains at native 48 kHz; the existing preallocated adaptive bridge also performs nominal conversion when the host uses 44.1 kHz.

The Behringer callback is the master realtime callback. Each cycle converts the two active UMC inputs from `ASIOSTInt32LSB` to preallocated float32 ASIO buffers, calls the host, converts the four UMC outputs back to int32, and pushes JBL outputs 5/6 into a preallocated single-producer/single-consumer ring. No realtime path performs heap allocation, file or console I/O, logging, registry access, or blocking mutex operations.

The JBL bridge applies the configured gain and linearly resamples with a bounded adaptive ratio. Its target is the selected host buffer plus 112 frames, absorbing the observed paired KS completion bursts and the independent USB-clock offset. The bridge is prefilled before KS starts. Bridge underflow/overflow and KS transport failures increment the same shared xrun telemetry counter.

## JBL KS backend

`src/ks.cpp` enumerates physical KS audio/render filters and identifies the Zgmicro USB function by friendly name or VID 0AC8/PID BBF5. It opens the input-flow render pin exclusively at 48 kHz stereo PCM16.

Two 96-frame packets are permanently cycled. This geometry was validated directly without audible clicks. The thread uses MMCSS `Pro Audio` at critical priority. Filter discovery, pin creation, event creation, shared-memory mapping, packet allocation, and scratch allocation all occur before streaming.

The reported JBL path is the mode-specific bridge target plus one 96-frame packet already ahead in KS. This yields output latencies of 272, 336, 464, and 720 frames for the 64, 128, 256, and 512 modes respectively. Native UMC input latencies are 136, 200, 328, and 584 frames, producing reported round trips of 8.50, 11.17, 16.50, and 27.17 ms.

## WASAPI reference backend

`src/wasapi.cpp` retains endpoint enumeration, native-format probing, exact exclusive-buffer probing, event/timer diagnostics, and a standalone renderer. It is not used by the ASIO realtime output path. On this endpoint, low-period WASAPI completion signaling was bursty and clicked; direct KS is therefore the default.

## Telemetry and configuration

Sample rate, preferred buffer size, and JBL output gain are loaded from `%LOCALAPPDATA%\XtremeASIO\config.yaml`; defaults are 48 kHz, 64 samples, and -18 dB. Endpoint selection remains in `HKCU\Software\XtremeASIO`. A named shared-memory block carries callbacks, aggregate xruns, wait timeouts, I/O errors, late gaps, rate, KS packet size, and running state. Realtime code only updates atomic/interlocked counters; the diagnostic process reads and displays them.

## Deferred work

- Measured electrical/acoustic round-trip latency.
- User-selectable aggregate latency profiles after stability testing.
- Higher-quality drift resampling if listening tests reveal artifacts.
- Multi-device input beyond the UMC204HD.
- Installer, code signing, and a dedicated graphical control panel.