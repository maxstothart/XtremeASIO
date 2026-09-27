#include "xtreme/wasapi.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>

namespace {

enum class Signal { Sine, Click };

struct Generator {
  Signal signal{Signal::Sine};
  double phase{};
  double phase_step{};
  std::uint64_t frame{};
  std::uint32_t rate{};
  float amplitude{1.0f};

  static void Fill(void* opaque, float* output, std::uint32_t frames, bool) noexcept {
    auto& self = *static_cast<Generator*>(opaque);
    for (std::uint32_t i = 0; i < frames; ++i, ++self.frame) {
      float sample = 0.0f;
      if (self.signal == Signal::Sine) {
        sample = static_cast<float>(0.12 * self.amplitude * std::sin(self.phase));
        self.phase += self.phase_step;
        if (self.phase >= 2.0 * std::numbers::pi) self.phase -= 2.0 * std::numbers::pi;
      } else if (self.frame % self.rate == 0) {
        sample = 0.35f;
      }
      output[i * 2] = sample;
      output[i * 2 + 1] = sample;
    }
  }
};

BOOL WINAPI StopHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    return TRUE;
  }
  return FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    std::uint32_t requested_frames = 128;
    std::uint32_t seconds = 10;
    Signal signal = Signal::Sine;
    bool event_driven = true;
    bool silent = false;
    std::uint32_t requested_bits = 0;
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--frames" && i + 1 < argc) requested_frames = std::stoul(argv[++i]);
      else if (arg == L"--seconds" && i + 1 < argc) seconds = std::stoul(argv[++i]);
      else if (arg == L"--click") signal = Signal::Click;
      else if (arg == L"--sine") signal = Signal::Sine;
      else if (arg == L"--timer") event_driven = false;
      else if (arg == L"--silence") silent = true;
      else if (arg == L"--bits" && i + 1 < argc) requested_bits = std::stoul(argv[++i]);
      else {
        std::wcerr << L"usage: wasapi_test [--frames N] [--seconds N] [--sine|--click] [--timer] [--silence] [--bits 16|24|32]\n";
        return 1;
      }
    }
    const auto endpoint = xtreme::SelectEndpoint();
    const auto probe = xtreme::ProbeEndpoint(endpoint);
    if (probe.formats.empty()) throw std::runtime_error("No tested exclusive stereo format is supported");
    const auto preferred = std::find_if(probe.formats.begin(), probe.formats.end(), [requested_bits](const auto& f) {
      return f.wave.Format.nSamplesPerSec == 48000 &&
             (requested_bits == 0 || f.wave.Format.wBitsPerSample == requested_bits);
    });
    const auto& format = preferred != probe.formats.end() ? *preferred : probe.formats.front();
    if (event_driven && requested_frames != 192 && requested_frames != 240 && requested_frames != 256 && requested_frames != 480 && std::find(probe.supported_buffer_frames.begin(), probe.supported_buffer_frames.end(),
                  requested_frames) == probe.supported_buffer_frames.end()) {
      throw std::runtime_error("Requested frame count was not accepted exactly by the endpoint");
    }
    Generator generator;
    generator.signal = signal;
    generator.amplitude = silent ? 0.0f : 1.0f;
    generator.rate = format.wave.Format.nSamplesPerSec;
    generator.phase_step = 2.0 * std::numbers::pi * 440.0 / generator.rate;
    xtreme::ExclusiveRenderer renderer;
    renderer.Open(endpoint, format, requested_frames, &Generator::Fill, &generator, event_driven);
    std::wcout << L"Opening " << endpoint.name << L" at " << format.label
               << L", " << renderer.BufferFrames() << L" frames; reported stream latency "
               << renderer.StreamLatencyFrames() << L" frames.\n";
    renderer.Start();
    Sleep(seconds * 1000);
    renderer.Stop();
    std::wcout << L"Complete. callbacks=" << renderer.Callbacks() << L" xruns=" << renderer.Xruns() << L'\n';
    return renderer.Xruns() == 0 ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}

