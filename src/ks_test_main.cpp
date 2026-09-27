#include <windows.h>
#include <avrt.h>
#include <winioctl.h>
#include <mmreg.h>
#include <setupapi.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ && value_ != INVALID_HANDLE_VALUE;
  }
  HANDLE release() noexcept { return std::exchange(value_, nullptr); }
  void reset(HANDLE value = nullptr) noexcept {
    if (*this) CloseHandle(value_);
    value_ = value;
  }
private:
  HANDLE value_{};
};

struct Device {
  std::wstring path;
  std::wstring name;
};

std::wstring Lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

std::vector<Device> EnumerateCategory(const GUID& category) {
  std::vector<Device> result;
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
      SetupDiGetDeviceRegistryPropertyW(info, &device_data, SPDRP_DEVICEDESC,
        nullptr, reinterpret_cast<PBYTE>(name.data()),
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

bool SetState(HANDLE pin, KSSTATE state) {
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

Handle CreateRenderPin(HANDLE filter, ULONG pin_id) {
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
  request.format.WaveFormatEx.nSamplesPerSec = 48000;
  request.format.WaveFormatEx.wBitsPerSample = 16;
  request.format.WaveFormatEx.nBlockAlign = 4;
  request.format.WaveFormatEx.nAvgBytesPerSec = 192000;
  HANDLE pin_handle{};
  const DWORD status = KsCreatePin(filter, &request.connect,
                                   GENERIC_READ | GENERIC_WRITE, &pin_handle);
  if (status != ERROR_SUCCESS) SetLastError(status);
  return Handle(status == ERROR_SUCCESS ? pin_handle : nullptr);
}

struct Packet {
  std::vector<std::int16_t> samples;
  KSSTREAM_HEADER header{};
  OVERLAPPED overlapped{};
  Handle event;
  std::chrono::steady_clock::time_point submitted{};
};

void FillSignal(Packet& packet, bool sine, double gain_db, double& phase) {
  if (!sine) {
    std::fill(packet.samples.begin(), packet.samples.end(), std::int16_t{0});
    return;
  }
  const double amplitude = std::pow(10.0, gain_db / 20.0);
  constexpr double kStep = 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
  for (std::size_t frame = 0; frame < packet.samples.size() / 2; ++frame) {
    const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * amplitude * 32767.0));
    packet.samples[frame * 2] = sample;
    packet.samples[frame * 2 + 1] = sample;
    phase += kStep;
    if (phase >= 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
  }
}

bool Submit(HANDLE pin, Packet& packet, bool sine, double gain_db, double& phase) {
  FillSignal(packet, sine, gain_db, phase);
  ResetEvent(packet.event.get());
  packet.overlapped = {};
  packet.overlapped.hEvent = packet.event.get();
  packet.header = {};
  packet.header.Size = sizeof(packet.header);
  packet.header.Data = packet.samples.data();
  packet.header.FrameExtent = static_cast<ULONG>(packet.samples.size() * sizeof(std::int16_t));
  packet.header.DataUsed = packet.header.FrameExtent;
  packet.header.PresentationTime.Numerator = 1;
  packet.header.PresentationTime.Denominator = 1;
  packet.submitted = std::chrono::steady_clock::now();
  DWORD bytes = 0;
  if (DeviceIoControl(pin, IOCTL_KS_WRITE_STREAM, nullptr, 0, &packet.header,
                      sizeof(packet.header), &bytes, &packet.overlapped)) return true;
  return GetLastError() == ERROR_IO_PENDING;
}

int RunPin(HANDLE pin, std::uint32_t frames, unsigned seconds, unsigned packet_count,
           bool sine, double gain_db) {
  DWORD task_index = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
  std::vector<Packet> packets(packet_count);
  for (auto& packet : packets) {
    packet.samples.assign(static_cast<std::size_t>(frames) * 2, 0);
    packet.event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!packet.event) throw std::runtime_error("CreateEvent failed");
  }
  if (!SetState(pin, KSSTATE_ACQUIRE) || !SetState(pin, KSSTATE_PAUSE)) {
    throw std::runtime_error("Could not prepare KS pin");
  }
  double phase = 0.0;
  for (auto& packet : packets) {
    if (!Submit(pin, packet, sine, gain_db, phase)) throw std::runtime_error("Initial KS write failed");
  }
  if (!SetState(pin, KSSTATE_RUN)) throw std::runtime_error("Could not run KS pin");

  std::vector<HANDLE> events(packets.size());
  for (std::size_t i = 0; i < packets.size(); ++i) events[i] = packets[i].event.get();
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  const double period_ms = static_cast<double>(frames) * 1000.0 / 48000.0;
  std::uint64_t completions = 0;
  std::uint64_t late = 0;
  double maximum_ms = 0.0;
  double maximum_gap_ms = 0.0;
  std::chrono::steady_clock::time_point last_completion{};
  while (std::chrono::steady_clock::now() < end) {
    const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                               FALSE, 1000);
    if (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + events.size()) {
      ++late;
      continue;
    }
    auto& packet = packets[wait - WAIT_OBJECT_0];
    DWORD bytes = 0;
    if (!GetOverlappedResult(pin, &packet.overlapped, &bytes, FALSE)) ++late;
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>(now - packet.submitted).count();
    maximum_ms = std::max(maximum_ms, elapsed);
    if (completions >= packets.size()) {
      const double gap = std::chrono::duration<double, std::milli>(now - last_completion).count();
      maximum_gap_ms = std::max(maximum_gap_ms, gap);
      if (gap > period_ms * 1.5) ++late;
    }
    last_completion = now;
    ++completions;
    if (!Submit(pin, packet, sine, gain_db, phase)) {
      ++late;
      break;
    }
  }
  SetState(pin, KSSTATE_STOP);
  CancelIoEx(pin, nullptr);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  std::wcout << L"KS result: frames=" << frames << L" packets=" << packets.size()
             << L" completions=" << completions << L" late_gaps=" << late
             << L" max_gap_ms=" << maximum_gap_ms
             << L" max_packet_roundtrip_ms=" << maximum_ms << L'\n';
  return late == 0 ? 0 : 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    std::uint32_t frames = 128;
    unsigned seconds = 10;
    unsigned packet_count = 2;
    bool sine = false;
    double gain_db = -24.0;
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--frames" && i + 1 < argc) frames = std::stoul(argv[++i]);
      else if (arg == L"--seconds" && i + 1 < argc) seconds = std::stoul(argv[++i]);
      else if (arg == L"--packets" && i + 1 < argc) packet_count = std::stoul(argv[++i]);
      else if (arg == L"--sine") sine = true;
      else if (arg == L"--gain-db" && i + 1 < argc) gain_db = std::stod(argv[++i]);
      else throw std::invalid_argument("Usage: ks_test [--frames N] [--seconds N] [--packets 1..8] [--sine] [--gain-db -96..0]");
    }
    if (packet_count < 1 || packet_count > 8) throw std::invalid_argument("packets must be 1..8");
    if (!std::isfinite(gain_db) || gain_db < -96.0 || gain_db > 0.0) {
      throw std::invalid_argument("gain-db must be -96..0");
    }
    std::vector<Device> devices;
    for (const GUID* category : {&KSCATEGORY_AUDIO, &KSCATEGORY_RENDER}) {
      for (auto& device : EnumerateCategory(*category)) {
        if (std::none_of(devices.begin(), devices.end(), [&](const Device& existing) {
              return existing.path == device.path;
            })) devices.push_back(std::move(device));
      }
    }
    for (const auto& device : devices) {
      if (Lower(device.name).find(L"zgmicro") == std::wstring::npos &&
          Lower(device.path).find(L"vid_0ac8&pid_bbf5") == std::wstring::npos) continue;
      std::wcout << L"KS filter: " << device.name << L"\n" << device.path << L'\n';
      Handle filter(CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
      if (!filter) {
        std::wcout << L"  open failed: " << GetLastError() << L'\n';
        continue;
      }
      KSPROPERTY count_request{KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET};
      ULONG pin_count = 0;
      DWORD bytes = 0;
      if (!DeviceIoControl(filter.get(), IOCTL_KS_PROPERTY, &count_request,
                           sizeof(count_request), &pin_count, sizeof(pin_count), &bytes, nullptr)) {
        std::wcout << L"  pin query failed: " << GetLastError() << L'\n';
        continue;
      }
      for (ULONG pin_id = 0; pin_id < pin_count; ++pin_id) {
        KSPIN_DATAFLOW flow{};
        KSPIN_COMMUNICATION communication{};
        if (!PinProperty(filter.get(), pin_id, KSPROPERTY_PIN_DATAFLOW, flow) ||
            !PinProperty(filter.get(), pin_id, KSPROPERTY_PIN_COMMUNICATION, communication)) continue;
        std::wcout << L"  pin " << pin_id << L" flow=" << flow
                   << L" communication=" << communication << L'\n';
        if (flow != KSPIN_DATAFLOW_IN ||
            (communication != KSPIN_COMMUNICATION_SINK &&
             communication != KSPIN_COMMUNICATION_BOTH)) continue;
        Handle pin = CreateRenderPin(filter.get(), pin_id);
        if (!pin) {
          std::wcout << L"    48 kHz PCM16 pin creation failed: " << GetLastError() << L'\n';
          continue;
        }
        if (sine) std::wcout << L"    streaming 440 Hz at " << gain_db << L" dBFS directly through KS\n";
        else std::wcout << L"    streaming silence directly through KS\n";
        return RunPin(pin.get(), frames, seconds, packet_count, sine, gain_db);
      }
    }
    std::wcerr << L"No usable Zgmicro KS render pin found.\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "ks_test: " << error.what() << '\n';
    return 1;
  }
}