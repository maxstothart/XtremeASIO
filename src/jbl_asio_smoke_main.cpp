#include "xtreme/wasapi.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>

#include "iasiodrv.h"

namespace {
constexpr CLSID kJblDriverClsid{0xa9a37f2d, 0x6e4b, 0x4c5b,
  {0x9a, 0x1d, 0x2e, 0x8f, 0x7b, 0x6c, 0x4d, 0x10}};
ASIOBufferInfo g_buffers[2]{};
long g_frames{};
double g_phase{-1.0};
double g_amplitude{};

void Fill(long index) noexcept {
  auto* left = static_cast<float*>(g_buffers[0].buffers[index]);
  auto* right = static_cast<float*>(g_buffers[1].buffers[index]);
  for (long frame = 0; frame < g_frames; ++frame) {
    float sample = 0.0f;
    if (g_phase >= 0.0) {
      sample = static_cast<float>(g_amplitude * std::sin(g_phase));
      g_phase += 2.0 * std::numbers::pi * 440.0 / 48000.0;
      if (g_phase >= 2.0 * std::numbers::pi) g_phase -= 2.0 * std::numbers::pi;
    }
    left[frame] = right[frame] = sample;
  }
}
void BufferSwitch(long index, ASIOBool) { Fill(index); }
void RateChanged(ASIOSampleRate) {}
long AsioMessage(long selector, long, void*, double*) {
  return selector == kAsioSupportsTimeInfo ? 1 : 0;
}
ASIOTime* BufferSwitchTimeInfo(ASIOTime*, long index, ASIOBool) {
  Fill(index);
  return nullptr;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  const wchar_t* dll_path = argc > 1 ? argv[1] : L"XtremeASIO_JBL.dll";
  const bool tone = argc > 2 && std::wstring(argv[2]) == L"tone";
  const int seconds = argc > 3 ? std::clamp(std::stoi(argv[3]), 1, 30) : 8;
  if (tone) {
    const double target_db = argc > 4 ? std::stod(argv[4]) : -36.0;
    const double host_db = std::min(0.0, target_db - xtreme::ReadOutputGainDb());
    g_amplitude = std::pow(10.0, host_db / 20.0);
    g_phase = 0.0;
  }

  HMODULE module = LoadLibraryW(dll_path);
  if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 1; }
  using GetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
  const auto get_class = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
  IClassFactory* factory{};
  IASIO* driver{};
  int result = 1;
  if (get_class && SUCCEEDED(get_class(kJblDriverClsid, IID_IClassFactory,
      reinterpret_cast<void**>(&factory))) &&
      SUCCEEDED(factory->CreateInstance(nullptr, kJblDriverClsid,
                                        reinterpret_cast<void**>(&driver))) &&
      driver->init(nullptr) == ASIOTrue) {
    long inputs{}, outputs{}, minimum{}, maximum{}, preferred{}, granularity{};
    ASIOSampleRate rate{};
    driver->getChannels(&inputs, &outputs);
    driver->getBufferSize(&minimum, &maximum, &preferred, &granularity);
    driver->getSampleRate(&rate);
    std::cout << "channels=" << inputs << '/' << outputs << " buffers=" << minimum << ".."
              << maximum << " preferred=" << preferred << " rate=" << rate << '\n';
    g_frames = preferred;
    for (long channel = 0; channel < 2; ++channel) {
      g_buffers[channel].isInput = ASIOFalse;
      g_buffers[channel].channelNum = channel;
    }
    ASIOCallbacks callbacks{&BufferSwitch, &RateChanged, &AsioMessage, &BufferSwitchTimeInfo};
    if (driver->createBuffers(g_buffers, 2, preferred, &callbacks) == ASE_OK) {
      Fill(1);
      if (driver->start() == ASE_OK) {
        Sleep(static_cast<DWORD>(seconds * 1000));
        driver->stop();
        const auto telemetry = xtreme::ReadTelemetry();
        long input_latency{}, output_latency{};
        driver->getLatencies(&input_latency, &output_latency);
        std::cout << "callbacks=" << telemetry.callbacks << " xruns=" << telemetry.xruns
                  << " timeouts=" << telemetry.wait_timeouts
                  << " io_errors=" << telemetry.io_errors
                  << " late_gaps=" << telemetry.late_gaps
                  << " output_latency_frames=" << output_latency
                  << " gain_db=" << xtreme::ReadOutputGainDb() << '\n';
        result = telemetry.xruns == 0 && telemetry.callbacks >
          static_cast<std::uint64_t>(seconds * 480) ? 0 : 2;
      }
      driver->disposeBuffers();
    }
  } else if (driver) {
    char error[124]{};
    driver->getErrorMessage(error);
    std::cerr << "Driver init failed: " << error << '\n';
  }
  if (driver) driver->Release();
  if (factory) factory->Release();
  FreeLibrary(module);
  return result;
}