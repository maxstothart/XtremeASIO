#include "xtreme/wasapi.hpp"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <numbers>
#include "iasiodrv.h"

namespace {
constexpr CLSID kDriverClsid{0x2d926c13, 0xf819, 0x4f32,
  {0xbd, 0x5d, 0x81, 0x48, 0xe8, 0x59, 0x13, 0xab}};
ASIOBufferInfo g_buffers[8]{};
long g_frames{};
double g_phase{};
double g_amplitude{};
double g_sample_rate{48000.0};
void Fill(long index) noexcept {
  // First two buffers are UMC inputs; outputs are UMC 1-4 then JBL L/R.
  auto* left = static_cast<float*>(g_buffers[6].buffers[index]);
  auto* right = static_cast<float*>(g_buffers[7].buffers[index]);
  for (long channel = 2; channel < 6; ++channel) {
    std::fill_n(static_cast<float*>(g_buffers[channel].buffers[index]), g_frames, 0.0f);
  }
  if (g_phase < 0.0) {
    std::fill_n(left, g_frames, 0.0f);
    std::fill_n(right, g_frames, 0.0f);
    return;
  }
  for (long frame = 0; frame < g_frames; ++frame) {
    const auto sample = static_cast<float>(g_amplitude * std::sin(g_phase));
    g_phase += 2.0 * std::numbers::pi * 440.0 / g_sample_rate;
    if (g_phase >= 2.0 * std::numbers::pi) g_phase -= 2.0 * std::numbers::pi;
    left[frame] = right[frame] = sample;
  }
}
void BufferSwitch(long index, ASIOBool) { Fill(index); }
void RateChanged(ASIOSampleRate) {}
long AsioMessage(long, long, void*, double*) { return 0; }
ASIOTime* BufferSwitchTimeInfo(ASIOTime*, long index, ASIOBool) { Fill(index); return nullptr; }
}  // namespace

int wmain(int argc, wchar_t** argv) {
  g_phase = -1.0;
  int seconds = 10;
  long requested_frames = 0;
  const wchar_t* dll_path = argc > 1 ? argv[1] : L"XtremeASIO.dll";
  if (argc > 2 && std::wstring(argv[2]) == L"tone") {
    const double requested_output_db = argc > 3 ? std::stod(argv[3]) : -36.0;
    seconds = argc > 4 ? std::clamp(std::stoi(argv[4]), 1, 30) : 10;
    requested_frames = argc > 5 ? std::stol(argv[5]) : 0;
    const double host_db = std::min(0.0, requested_output_db - xtreme::ReadOutputGainDb());
    g_amplitude = std::pow(10.0, host_db / 20.0);
    g_phase = 0.0;
    std::cerr << "tone target=" << requested_output_db << " dBFS host=" << host_db
              << " dBFS duration=" << seconds << "s\n";
  } else if (argc > 2 && std::wstring(argv[2]) == L"silent") {
    seconds = argc > 3 ? std::clamp(std::stoi(argv[3]), 1, 60) : 20;
    requested_frames = argc > 4 ? std::stol(argv[4]) : 0;
  }
  HMODULE module = LoadLibraryW(dll_path);
  if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 1; }
  std::cerr << "loaded DLL\n";
  using GetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
  const auto get_class = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
  IClassFactory* factory{};
  IASIO* driver{};
  int exit_code = 1;
  std::cerr << "resolved export\n";
  if (get_class && SUCCEEDED(get_class(kDriverClsid, IID_IClassFactory,
      reinterpret_cast<void**>(&factory))) &&
      SUCCEEDED(factory->CreateInstance(nullptr, kDriverClsid, reinterpret_cast<void**>(&driver))) &&
      driver->init(nullptr) == ASIOTrue) {
    std::cerr << "driver initialized\n";
    long inputs = 0, outputs = 0, minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    driver->getChannels(&inputs, &outputs);
    ASIOSampleRate actual_rate = 0;
    driver->getSampleRate(&actual_rate);
    g_sample_rate = actual_rate;
    driver->getBufferSize(&minimum, &maximum, &preferred, &granularity);
    std::cout << "channels=" << inputs << '/' << outputs << " buffers=" << minimum << ".."
              << maximum << " preferred=" << preferred << " rate=" << g_sample_rate << " granularity=" << granularity << '\n';
    g_frames = requested_frames ? requested_frames : preferred;
    std::cout << "selected_buffer=" << g_frames << '\n';
    for (long channel = 0; channel < 2; ++channel) {
      g_buffers[channel].isInput = ASIOTrue;
      g_buffers[channel].channelNum = channel;
    }
    for (long channel = 0; channel < 6; ++channel) {
      g_buffers[channel + 2].isInput = ASIOFalse;
      g_buffers[channel + 2].channelNum = channel;
    }
    ASIOCallbacks callbacks{&BufferSwitch, &RateChanged, &AsioMessage, &BufferSwitchTimeInfo};
    if (driver->createBuffers(g_buffers, 8, g_frames, &callbacks) == ASE_OK) {
      std::cerr << "buffers created\n";
      Fill(1);
      if (driver->start() == ASE_OK) {
        std::cerr << "started\n";
        for (int second = 1; second <= seconds; ++second) {
          Sleep(1000);
          const auto current = xtreme::ReadTelemetry();
          std::cerr << "t=" << second << "s callbacks=" << current.callbacks
                    << " xruns=" << current.xruns << '\n';
        }
        driver->stop();
        std::cerr << "stopped\n";
        const auto telemetry = xtreme::ReadTelemetry();
        long input_latency = 0, output_latency = 0;
        driver->getLatencies(&input_latency, &output_latency);
        std::cout << "callbacks=" << telemetry.callbacks << " xruns=" << telemetry.xruns
                  << " timeouts=" << telemetry.wait_timeouts
                  << " io_errors=" << telemetry.io_errors
                  << " late_gaps=" << telemetry.late_gaps
                  << " concealed_frames=" << telemetry.concealed_frames
                  << " max_concealed=" << telemetry.max_concealed_frames
                  << " input_latency_frames=" << input_latency
                  << " output_latency_frames=" << output_latency
                  << " gain_db=" << xtreme::ReadOutputGainDb() << '\n';
        exit_code = telemetry.xruns == 0 && telemetry.callbacks > static_cast<std::uint64_t>(seconds * (g_sample_rate / 96.0) * 0.96) ? 0 : 2;
      }
      driver->disposeBuffers();
      std::cerr << "buffers disposed\n";
    }
  }
  std::cerr << "releasing\n";
  if (driver) driver->Release();
  if (factory) factory->Release();
  FreeLibrary(module);
  return exit_code;
}
