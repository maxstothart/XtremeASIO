#include "xtreme/ks.hpp"

#include <avrt.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace xtreme {
namespace {

constexpr wchar_t kTelemetryName[] = L"Local\\XtremeASIO.Telemetry.v1";

struct SharedTelemetry {
  volatile LONG64 xruns;
  volatile LONG64 callbacks;
  volatile LONG64 wait_timeouts;
  volatile LONG64 io_errors;
  volatile LONG64 late_gaps;
  volatile LONG64 concealed_frames;
  volatile LONG max_concealed_frames;
  volatile LONG sample_rate;
  volatile LONG buffer_frames;
  volatile LONG running;
};

struct FilterInfo {
  std::wstring path;
  std::wstring name;
};

std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

std::vector<FilterInfo> Enumerate(const GUID& category) {
  std::vector<FilterInfo> result;
  const HDEVINFO info = SetupDiGetClassDevsW(&category, nullptr, nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (info == INVALID_HANDLE_VALUE) return result;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data{sizeof(interface_data)};
    if (!SetupDiEnumDeviceInterfaces(info, nullptr, &category, index, &interface_data)) {
      if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
      continue;
    }
    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &interface_data, nullptr, 0, &required, nullptr);
    if (!required) continue;
    std::vector<std::byte> storage(required);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA device_data{sizeof(device_data)};
    if (!SetupDiGetDeviceInterfaceDetailW(info, &interface_data, detail, required,
                                           nullptr, &device_data)) continue;
    std::array<wchar_t, 512> name{};
    if (!SetupDiGetDeviceRegistryPropertyW(info, &device_data, SPDRP_FRIENDLYNAME,
        nullptr, reinterpret_cast<PBYTE>(name.data()),
        static_cast<DWORD>(name.size() * sizeof(wchar_t)), nullptr)) {
      SetupDiGetDeviceRegistryPropertyW(info, &device_data, SPDRP_DEVICEDESC, nullptr,
        reinterpret_cast<PBYTE>(name.data()),
        static_cast<DWORD>(name.size() * sizeof(wchar_t)), nullptr);
    }
    result.push_back({detail->DevicePath, name.data()});
  }
  SetupDiDestroyDeviceInfoList(info);
  return result;
}

template <typename T>
bool PinProperty(HANDLE filter, ULONG pin, ULONG id, T& value) {
  KSP_PIN request{{KSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET}, pin, 0};
  DWORD bytes = 0;
  return DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
                         &value, sizeof(value), &bytes, nullptr) != FALSE;
}

bool SetState(HANDLE pin, KSSTATE state) noexcept {
  KSPROPERTY request{KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE,
                     KSPROPERTY_TYPE_SET};
  DWORD bytes = 0;
  return DeviceIoControl(pin, IOCTL_KS_PROPERTY, &request, sizeof(request),
                         &state, sizeof(state), &bytes, nullptr) != FALSE;
}

struct PinRequest {
  KSPIN_CONNECT connect{};
  KSDATAFORMAT_WAVEFORMATEX format{};
};

HANDLE CreateRenderPin(HANDLE filter, ULONG pin_id, std::uint32_t sample_rate) {
  PinRequest request{};
  request.connect.Interface.Set = KSINTERFACESETID_Standard;
  request.connect.Interface.Id = KSINTERFACE_STANDARD_STREAMING;
  request.connect.Medium.Set = KSMEDIUMSETID_Standard;
  request.connect.Medium.Id = KSMEDIUM_STANDARD_DEVIO;
  request.connect.PinId = pin_id;
  request.connect.Priority.PriorityClass = KSPRIORITY_EXCLUSIVE;
  request.connect.Priority.PrioritySubClass = 1;
  request.format.DataFormat.FormatSize = sizeof(request.format);
  request.format.DataFormat.SampleSize = 4;
  request.format.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
  request.format.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  request.format.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
  request.format.WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
  request.format.WaveFormatEx.nChannels = 2;
  request.format.WaveFormatEx.nSamplesPerSec = sample_rate;
  request.format.WaveFormatEx.wBitsPerSample = 16;
  request.format.WaveFormatEx.nBlockAlign = 4;
  request.format.WaveFormatEx.nAvgBytesPerSec = sample_rate * 4;
  HANDLE pin{};
  const DWORD status = KsCreatePin(filter, &request.connect,
                                   GENERIC_READ | GENERIC_WRITE, &pin);
  if (status != ERROR_SUCCESS) SetLastError(status);
  return status == ERROR_SUCCESS ? pin : nullptr;
}

}  // namespace

KsRenderer::~KsRenderer() { Close(); }

void KsRenderer::Open(std::uint32_t frames, FillCallback callback, void* context,
                      std::uint32_t sample_rate) {
  Close();
  if (!callback || frames == 0 || sample_rate < 8000) throw std::invalid_argument("Invalid KS callback, buffer size, or sample rate");
  std::vector<FilterInfo> filters;
  for (const GUID* category : {&KSCATEGORY_AUDIO, &KSCATEGORY_RENDER}) {
    for (auto& item : Enumerate(*category)) {
      if (std::none_of(filters.begin(), filters.end(), [&](const auto& existing) {
            return existing.path == item.path;
          })) filters.push_back(std::move(item));
    }
  }
  DWORD last_error = ERROR_NOT_FOUND;
  for (const auto& item : filters) {
    if (Lower(item.name).find(L"zgmicro") == std::wstring::npos &&
        Lower(item.path).find(L"vid_0ac8&pid_bbf5") == std::wstring::npos) continue;
    HANDLE filter = CreateFileW(item.path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (filter == INVALID_HANDLE_VALUE) {
      last_error = GetLastError();
      continue;
    }
    KSPROPERTY count_request{KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET};
    ULONG pin_count = 0;
    DWORD bytes = 0;
    if (!DeviceIoControl(filter, IOCTL_KS_PROPERTY, &count_request, sizeof(count_request),
                         &pin_count, sizeof(pin_count), &bytes, nullptr)) {
      last_error = GetLastError();
      CloseHandle(filter);
      continue;
    }
    for (ULONG pin_id = 0; pin_id < pin_count; ++pin_id) {
      KSPIN_DATAFLOW flow{};
      KSPIN_COMMUNICATION communication{};
      if (!PinProperty(filter, pin_id, KSPROPERTY_PIN_DATAFLOW, flow) ||
          !PinProperty(filter, pin_id, KSPROPERTY_PIN_COMMUNICATION, communication) ||
          flow != KSPIN_DATAFLOW_IN ||
          (communication != KSPIN_COMMUNICATION_SINK &&
           communication != KSPIN_COMMUNICATION_BOTH)) continue;
      HANDLE pin = CreateRenderPin(filter, pin_id, sample_rate);
      if (pin) {
        filter_ = filter;
        pin_ = pin;
        break;
      }
      last_error = GetLastError();
    }
    if (pin_) break;
    CloseHandle(filter);
  }
  if (!pin_) throw std::runtime_error("Could not open the Zgmicro KS render pin: " +
                                      std::to_string(last_error));

  frames_ = frames;
  sample_rate_ = sample_rate;
  callback_ = callback;
  callback_context_ = context;
  scratch_.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
  for (auto& packet : packets_) {
    packet.samples.assign(static_cast<std::size_t>(frames) * 2, 0);
    packet.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!packet.event) {
      const DWORD error = GetLastError();
      Close();
      throw std::runtime_error("Could not create KS packet event: " + std::to_string(error));
    }
  }
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop_event_ || !ready_event_) {
    const DWORD error = GetLastError();
    Close();
    throw std::runtime_error("Could not create KS control events: " + std::to_string(error));
  }
  waits_ = {stop_event_, packets_[0].event, packets_[1].event};
  telemetry_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
    static_cast<DWORD>(sizeof(SharedTelemetry)), kTelemetryName);
  if (telemetry_mapping_) {
    telemetry_view_ = MapViewOfFile(telemetry_mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                    sizeof(SharedTelemetry));
  }
  xruns_.store(0, std::memory_order_relaxed);
  callbacks_.store(0, std::memory_order_relaxed);
}

void KsRenderer::Start() {
  if (!IsOpen() || thread_.joinable()) throw std::logic_error("KS renderer is not open or already started");
  ResetEvent(stop_event_);
  ResetEvent(ready_event_);
  thread_start_error_ = ERROR_GEN_FAILURE;
  thread_ = std::thread([this] { ThreadMain(); });
  if (WaitForSingleObject(ready_event_, 5000) != WAIT_OBJECT_0 || thread_start_error_ != ERROR_SUCCESS) {
    const DWORD error = thread_start_error_;
    Stop();
    throw std::runtime_error("KS realtime thread startup failed: " + std::to_string(error));
  }
}

void KsRenderer::Stop() noexcept {
  if (stop_event_) SetEvent(stop_event_);
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_release);
}

void KsRenderer::Close() noexcept {
  Stop();
  if (pin_) CloseHandle(pin_);
  if (filter_) CloseHandle(filter_);
  for (auto& packet : packets_) {
    if (packet.event) CloseHandle(packet.event);
    packet.event = nullptr;
    packet.samples.clear();
  }
  if (stop_event_) CloseHandle(stop_event_);
  if (ready_event_) CloseHandle(ready_event_);
  if (telemetry_view_) UnmapViewOfFile(telemetry_view_);
  if (telemetry_mapping_) CloseHandle(telemetry_mapping_);
  filter_ = pin_ = stop_event_ = ready_event_ = telemetry_mapping_ = nullptr;
  telemetry_view_ = nullptr;
  scratch_.clear();
  callback_ = nullptr;
  callback_context_ = nullptr;
  frames_ = 0;
  sample_rate_ = 0;
}

bool KsRenderer::Submit(Packet& packet) noexcept {
  ResetEvent(packet.event);
  packet.overlapped = {};
  packet.overlapped.hEvent = packet.event;
  packet.header = {};
  packet.header.Size = sizeof(packet.header);
  packet.header.Data = packet.samples.data();
  packet.header.FrameExtent = static_cast<ULONG>(packet.samples.size() * sizeof(std::int16_t));
  packet.header.DataUsed = packet.header.FrameExtent;
  packet.header.PresentationTime.Numerator = 1;
  packet.header.PresentationTime.Denominator = 1;
  DWORD bytes = 0;
  if (DeviceIoControl(pin_, IOCTL_KS_WRITE_STREAM, nullptr, 0, &packet.header,
                      sizeof(packet.header), &bytes, &packet.overlapped)) return true;
  return GetLastError() == ERROR_IO_PENDING;
}

bool KsRenderer::RenderAndSubmit(Packet& packet) noexcept {
  std::fill(scratch_.begin(), scratch_.end(), 0.0f);
  callback_(callback_context_, scratch_.data(), frames_, false);
  for (std::size_t i = 0; i < packet.samples.size(); ++i) {
    const float sample = std::clamp(scratch_[i], -1.0f, std::nextafter(1.0f, 0.0f));
    packet.samples[i] = static_cast<std::int16_t>(std::lrintf(sample * 32768.0f));
  }
  callbacks_.fetch_add(1, std::memory_order_relaxed);
  return Submit(packet);
}

void KsRenderer::ThreadMain() noexcept {
  DWORD task_index = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
  auto* telemetry = static_cast<SharedTelemetry*>(telemetry_view_);
  if (telemetry) {
    InterlockedExchange64(&telemetry->xruns, 0);
    InterlockedExchange64(&telemetry->callbacks, 0);
    InterlockedExchange64(&telemetry->wait_timeouts, 0);
    InterlockedExchange64(&telemetry->io_errors, 0);
    InterlockedExchange64(&telemetry->late_gaps, 0);
    InterlockedExchange64(&telemetry->concealed_frames, 0);
    InterlockedExchange(&telemetry->max_concealed_frames, 0);
    InterlockedExchange(&telemetry->sample_rate, static_cast<LONG>(sample_rate_));
    InterlockedExchange(&telemetry->buffer_frames, static_cast<LONG>(frames_));
    InterlockedExchange(&telemetry->running, 0);
  }
  thread_start_error_ = ERROR_SUCCESS;
  if (!SetState(pin_, KSSTATE_ACQUIRE) || !SetState(pin_, KSSTATE_PAUSE)) {
    thread_start_error_ = GetLastError();
  }
  if (thread_start_error_ == ERROR_SUCCESS) {
    for (auto& packet : packets_) {
      std::fill(packet.samples.begin(), packet.samples.end(), std::int16_t{0});
      if (!Submit(packet)) {
        thread_start_error_ = GetLastError();
        break;
      }
    }
  }
  if (thread_start_error_ == ERROR_SUCCESS && !SetState(pin_, KSSTATE_RUN)) {
    thread_start_error_ = GetLastError();
  }
  if (thread_start_error_ == ERROR_SUCCESS) {
    running_.store(true, std::memory_order_release);
    if (telemetry) InterlockedExchange(&telemetry->running, 1);
  }
  SetEvent(ready_event_);

  LARGE_INTEGER frequency{};
  LARGE_INTEGER previous{};
  QueryPerformanceFrequency(&frequency);
  std::uint64_t completed = 0;
  while (thread_start_error_ == ERROR_SUCCESS) {
    const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(waits_.size()), waits_.data(),
                                               FALSE, 10);
    if (wait == WAIT_OBJECT_0) break;
    if (wait < WAIT_OBJECT_0 + 1 || wait > WAIT_OBJECT_0 + 2) {
      // USB class-driver spin-up can delay the first primed silent packets.
      // It precedes all ASIO callbacks and is not a realtime underrun.
      if (wait == WAIT_TIMEOUT) {
        if (completed >= packets_.size() && telemetry) {
          InterlockedIncrement64(&telemetry->wait_timeouts);
        }
        continue;
      }
      if (telemetry) InterlockedIncrement64(&telemetry->io_errors);
      CountXrun();
      continue;
    }
    auto& packet = packets_[wait - WAIT_OBJECT_0 - 1];
    DWORD bytes = 0;
    if (!GetOverlappedResult(pin_, &packet.overlapped, &bytes, FALSE)) {
      if (telemetry) InterlockedIncrement64(&telemetry->io_errors);
      CountXrun();
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (completed >= packets_.size() && frequency.QuadPart > 0) {
      const auto elapsed_us = static_cast<std::uint64_t>(
        (now.QuadPart - previous.QuadPart) * 1'000'000LL / frequency.QuadPart);
      const auto queue_us = static_cast<std::uint64_t>(frames_) * packets_.size() * 1'000'000ULL / sample_rate_;
      if (elapsed_us > queue_us) {
        if (telemetry) InterlockedIncrement64(&telemetry->late_gaps);
        CountXrun();
      }
    }
    previous = now;
    ++completed;
    if (RenderAndSubmit(packet)) {
      if (telemetry) InterlockedIncrement64(&telemetry->callbacks);
    } else {
      if (telemetry) InterlockedIncrement64(&telemetry->io_errors);
      CountXrun();
    }
  }
  SetState(pin_, KSSTATE_STOP);
  CancelIoEx(pin_, nullptr);
  running_.store(false, std::memory_order_release);
  if (telemetry) InterlockedExchange(&telemetry->running, 0);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

void KsRenderer::ReportBridgeConcealment(std::uint32_t frames) noexcept {
  if (!frames) return;
  CountXrun();
  if (auto* telemetry = static_cast<SharedTelemetry*>(telemetry_view_)) {
    InterlockedAdd64(&telemetry->concealed_frames, static_cast<LONG64>(frames));
    LONG observed = InterlockedCompareExchange(&telemetry->max_concealed_frames, 0, 0);
    while (static_cast<LONG>(frames) > observed) {
      const LONG prior = InterlockedCompareExchange(
        &telemetry->max_concealed_frames, static_cast<LONG>(frames), observed);
      if (prior == observed) break;
      observed = prior;
    }
  }
}
void KsRenderer::CountXrun() noexcept {
  xruns_.fetch_add(1, std::memory_order_relaxed);
  if (auto* telemetry = static_cast<SharedTelemetry*>(telemetry_view_)) {
    InterlockedIncrement64(&telemetry->xruns);
  }
}

}  // namespace xtreme
