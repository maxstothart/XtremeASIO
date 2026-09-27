#include "xtreme/wasapi.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Usage() {
  std::wcout << L"XtremeASIO diagnostic\n"
             << L"  xtremeasio_diag list\n"
             << L"  xtremeasio_diag probe\n"
             << L"  xtremeasio_diag select [endpoint-index]\n"
             << L"  xtremeasio_diag config\n"
             << L"  xtremeasio_diag gain [dB from -96 to 0]\n"
             << L"  xtremeasio_diag xruns [--watch]\n";
}

void PrintTelemetry() {
  const auto value = xtreme::ReadTelemetry();
  std::wcout << L"running=" << value.running << L" rate=" << value.sample_rate
             << L" buffer=" << value.buffer_frames << L" callbacks=" << value.callbacks
             << L" xruns=" << value.xruns
             << L" timeouts=" << value.wait_timeouts
             << L" io_errors=" << value.io_errors
             << L" late_gaps=" << value.late_gaps
             << L" concealed_frames=" << value.concealed_frames
             << L" max_concealed=" << value.max_concealed_frames << L'\n';
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    const std::wstring command = argc > 1 ? argv[1] : L"probe";
    if (command == L"list") {
      const auto endpoints = xtreme::EnumerateRenderEndpoints();
      for (std::size_t i = 0; i < endpoints.size(); ++i) {
        std::wcout << L'[' << i << L"] " << endpoints[i].name
                   << L" state=0x" << std::hex << endpoints[i].state << std::dec
                   << L"\n    " << endpoints[i].id << L'\n';
      }
      return 0;
    }
    if (command == L"select") {
      const auto endpoints = xtreme::EnumerateRenderEndpoints();
      std::size_t index = 0;
      if (argc > 2) {
        index = std::stoul(argv[2]);
      } else {
        const auto it = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
          return endpoint.state == DEVICE_STATE_ACTIVE &&
                 endpoint.name.find(xtreme::kTargetEndpointName) != std::wstring::npos;
        });
        if (it == endpoints.end()) throw std::runtime_error("Zgmicro AUDIO endpoint not found");
        index = static_cast<std::size_t>(std::distance(endpoints.begin(), it));
      }
      if (index >= endpoints.size()) throw std::out_of_range("Endpoint index is out of range");
      if (!xtreme::WriteConfiguredEndpointId(endpoints[index].id)) {
        throw std::runtime_error("Could not save endpoint selection in HKCU");
      }
      std::wcout << L"Selected: " << endpoints[index].name << L'\n';
      return 0;
    }
    if (command == L"xruns") {
      const bool watch = argc > 2 && std::wstring(argv[2]) == L"--watch";
      do {
        PrintTelemetry();
        if (watch) std::this_thread::sleep_for(std::chrono::seconds(1));
      } while (watch);
      return 0;
    }
    if (command == L"config") {
      const auto settings = xtreme::ReadDriverSettings();
      std::wcout << L"Config: " << xtreme::SettingsFilePath()
                 << L"\nsample_rate: " << settings.sample_rate
                 << L"\npreferred_buffer_size: " << settings.preferred_buffer_size
                 << L"\njbl_gain_db: " << settings.jbl_gain_db << L'\n';
      return 0;
    }    if (command == L"gain") {
      if (argc > 2) {
        const double gain = std::stod(argv[2]);
        if (!xtreme::WriteOutputGainDb(gain)) {
          throw std::out_of_range("Gain must be between -96 and 0 dB");
        }
      }
      std::wcout << L"Output gain: " << xtreme::ReadOutputGainDb() << L" dB\n";
      return 0;
    }    if (command == L"probe") {
      const auto endpoint = xtreme::SelectEndpoint();
      const auto settings = xtreme::ReadDriverSettings();
      const auto probe = xtreme::ProbeEndpoint(endpoint, settings.sample_rate);
      std::wcout << L"Endpoint: " << endpoint.name << L"\nID: " << endpoint.id
                 << L"\nDefault period: " << xtreme::ReferenceTimeMs(probe.default_period)
                 << L" ms\nMinimum period: " << xtreme::ReferenceTimeMs(probe.minimum_period)
                 << L" ms\nExclusive stereo formats:\n";
      for (const auto& format : probe.formats) std::wcout << L"  " << format.label << L'\n';
      std::wcout << L"Exact supported candidate buffers (using preferred format):";
      for (const auto frames : probe.supported_buffer_frames) std::wcout << L' ' << frames;
      std::wcout << L"\n";
      PrintTelemetry();
      return probe.formats.empty() ? 2 : 0;
    }
    Usage();
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}

