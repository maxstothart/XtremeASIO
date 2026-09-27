#pragma once

#include <windows.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace xtreme {

class KsRenderer final {
public:
  using FillCallback = void (*)(void* context, float* interleaved_stereo,
                                std::uint32_t frames, bool priming) noexcept;

  KsRenderer() = default;
  ~KsRenderer();
  KsRenderer(const KsRenderer&) = delete;
  KsRenderer& operator=(const KsRenderer&) = delete;

  void Open(std::uint32_t frames, FillCallback callback, void* context,
            std::uint32_t sample_rate = 48000);
  void Start();
  void Stop() noexcept;
  void Close() noexcept;

  [[nodiscard]] bool IsOpen() const noexcept { return pin_ != nullptr; }
  [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint32_t BufferFrames() const noexcept { return frames_; }
  [[nodiscard]] std::uint32_t SampleRate() const noexcept { return sample_rate_; }
  [[nodiscard]] std::uint32_t StreamLatencyFrames() const noexcept { return frames_ * 2; }
  [[nodiscard]] std::uint64_t Xruns() const noexcept { return xruns_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t Callbacks() const noexcept { return callbacks_.load(std::memory_order_relaxed); }
  void ReportExternalXrun() noexcept { CountXrun(); }
  void ReportBridgeConcealment(std::uint32_t frames) noexcept;

private:
  struct Packet {
    std::vector<std::int16_t> samples;
    KSSTREAM_HEADER header{};
    OVERLAPPED overlapped{};
    HANDLE event{};
  };

  void ThreadMain() noexcept;
  bool Submit(Packet& packet) noexcept;
  bool RenderAndSubmit(Packet& packet) noexcept;
  void CountXrun() noexcept;

  HANDLE filter_{};
  HANDLE pin_{};
  HANDLE stop_event_{};
  HANDLE ready_event_{};
  HANDLE telemetry_mapping_{};
  void* telemetry_view_{};
  std::array<Packet, 2> packets_{};
  std::array<HANDLE, 3> waits_{};
  std::vector<float> scratch_;
  std::thread thread_;
  FillCallback callback_{};
  void* callback_context_{};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> xruns_{0};
  std::atomic<std::uint64_t> callbacks_{0};
  std::uint32_t frames_{};
  std::uint32_t sample_rate_{};
  DWORD thread_start_error_{ERROR_GEN_FAILURE};
};

}  // namespace xtreme
