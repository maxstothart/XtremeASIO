#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace xtreme {

inline constexpr wchar_t kTargetEndpointName[] = L"Zgmicro";
inline constexpr std::array<std::uint32_t, 5> kCandidateBufferFrames{32, 64, 96, 128, 256};

struct DriverSettings {
  std::uint32_t sample_rate{48000};
  std::uint32_t preferred_buffer_size{64};
  double jbl_gain_db{-18.0};
};

[[nodiscard]] std::wstring SettingsFilePath();
[[nodiscard]] DriverSettings ReadDriverSettings() noexcept;
bool WriteDriverSettings(const DriverSettings& settings) noexcept;
struct AudioFormat {
  WAVEFORMATEXTENSIBLE wave{};
  std::wstring label;
};

struct EndpointInfo {
  std::wstring id;
  std::wstring name;
  DWORD state{};
};

struct EndpointProbe {
  EndpointInfo endpoint;
  REFERENCE_TIME default_period{};
  REFERENCE_TIME minimum_period{};
  std::vector<AudioFormat> formats;
  std::vector<std::uint32_t> supported_buffer_frames;
};

struct TelemetrySnapshot {
  std::uint64_t xruns{};
  std::uint64_t callbacks{};
  std::uint64_t wait_timeouts{};
  std::uint64_t io_errors{};
  std::uint64_t late_gaps{};
  std::uint64_t concealed_frames{};
  std::uint32_t max_concealed_frames{};
  std::uint32_t sample_rate{};
  std::uint32_t buffer_frames{};
  std::uint32_t running{};
};

[[nodiscard]] std::vector<EndpointInfo> EnumerateRenderEndpoints();
[[nodiscard]] std::wstring ReadConfiguredEndpointId();
bool WriteConfiguredEndpointId(const std::wstring& id);
[[nodiscard]] double ReadOutputGainDb() noexcept;
bool WriteOutputGainDb(double gain_db) noexcept;
[[nodiscard]] EndpointInfo SelectEndpoint(const std::wstring& requested = {});
[[nodiscard]] EndpointProbe ProbeEndpoint(const EndpointInfo& endpoint,
                                          std::uint32_t preferred_rate = 48000);
[[nodiscard]] std::string HResultText(HRESULT hr);
[[nodiscard]] double ReferenceTimeMs(REFERENCE_TIME value) noexcept;
[[nodiscard]] TelemetrySnapshot ReadTelemetry() noexcept;

class ExclusiveRenderer final {
public:
  using FillCallback = void (*)(void* context, float* interleaved_stereo,
                                std::uint32_t frames, bool priming) noexcept;

  ExclusiveRenderer();
  ~ExclusiveRenderer();
  ExclusiveRenderer(const ExclusiveRenderer&) = delete;
  ExclusiveRenderer& operator=(const ExclusiveRenderer&) = delete;

  void Open(const EndpointInfo& endpoint, const AudioFormat& format,
            std::uint32_t requested_frames, FillCallback callback, void* context,
            bool event_driven = true);
  void Start();
  void Stop() noexcept;
  void Close() noexcept;

  [[nodiscard]] bool IsOpen() const noexcept { return audio_client_ != nullptr; }
  [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint32_t BufferFrames() const noexcept { return buffer_frames_; }
  [[nodiscard]] std::uint32_t SampleRate() const noexcept { return format_.wave.Format.nSamplesPerSec; }
  [[nodiscard]] std::uint32_t StreamLatencyFrames() const noexcept { return latency_frames_; }
  [[nodiscard]] std::uint64_t Xruns() const noexcept { return xruns_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t Callbacks() const noexcept { return callbacks_.load(std::memory_order_relaxed); }

private:
  void ThreadMain() noexcept;
  bool FillEndpointBuffer(std::uint32_t frames, bool priming) noexcept;
  void ConvertToDevice(BYTE* destination, std::uint32_t frames) noexcept;
  void CountXrun() noexcept;

  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioRenderClient> render_client_;
  Microsoft::WRL::ComPtr<IAudioClock> audio_clock_;
  AudioFormat format_{};
  FillCallback callback_{};
  void* callback_context_{};
  HANDLE audio_event_{};
  HANDLE stop_event_{};
  HANDLE ready_event_{};
  HANDLE safety_timer_{};
  HANDLE telemetry_mapping_{};
  void* telemetry_view_{};
  std::thread thread_;
  std::vector<float> scratch_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> xruns_{0};
  std::atomic<std::uint64_t> callbacks_{0};
  std::uint32_t buffer_frames_{};
  std::uint32_t callback_frames_{};
  std::uint32_t latency_frames_{};
  HRESULT thread_start_result_{E_FAIL};
  HRESULT open_com_result_{E_FAIL};
  bool event_driven_{true};
};

}  // namespace xtreme

