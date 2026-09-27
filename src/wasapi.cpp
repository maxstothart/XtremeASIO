#include "xtreme/wasapi.hpp"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propvarutil.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace xtreme {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\XtremeASIO";
constexpr wchar_t kEndpointValue[] = L"EndpointId";
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

class ComApartment {
public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
  [[nodiscard]] HRESULT result() const noexcept { return result_; }
private:
  HRESULT result_;
};

struct TelemetryMap {
  HANDLE mapping{};
  SharedTelemetry* data{};

  TelemetryMap(bool create) noexcept {
    mapping = create
      ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                           static_cast<DWORD>(sizeof(SharedTelemetry)), kTelemetryName)
      : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kTelemetryName);
    if (mapping) {
      data = static_cast<SharedTelemetry*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTelemetry)));
    }
  }
  ~TelemetryMap() {
    if (data) UnmapViewOfFile(data);
    if (mapping) CloseHandle(mapping);
  }
};

[[noreturn]] void ThrowHr(const char* operation, HRESULT hr) {
  std::ostringstream out;
  out << operation << " failed: " << HResultText(hr);
  throw std::runtime_error(out.str());
}

void CheckHr(HRESULT hr, const char* operation) {
  if (FAILED(hr)) ThrowHr(operation, hr);
}

ComPtr<IMMDeviceEnumerator> CreateEnumerator() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  CheckHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "CoCreateInstance(MMDeviceEnumerator)");
  return enumerator;
}

ComPtr<IMMDevice> DeviceFromId(const std::wstring& id) {
  auto enumerator = CreateEnumerator();
  ComPtr<IMMDevice> device;
  CheckHr(enumerator->GetDevice(id.c_str(), &device), "IMMDeviceEnumerator::GetDevice");
  return device;
}

std::wstring DeviceName(IMMDevice* device) {
  ComPtr<IPropertyStore> properties;
  CheckHr(device->OpenPropertyStore(STGM_READ, &properties), "IMMDevice::OpenPropertyStore");
  PROPVARIANT value;
  PropVariantInit(&value);
  const HRESULT hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
  std::wstring result;
  if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) result = value.pwszVal;
  PropVariantClear(&value);
  CheckHr(hr, "IPropertyStore::GetValue(PKEY_Device_FriendlyName)");
  return result;
}

AudioFormat MakeFormat(std::uint32_t rate, WORD bits, WORD valid_bits, bool floating) {
  AudioFormat result;
  auto& w = result.wave;
  w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  w.Format.nChannels = 2;
  w.Format.nSamplesPerSec = rate;
  w.Format.wBitsPerSample = bits;
  w.Format.nBlockAlign = static_cast<WORD>(2 * bits / 8);
  w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
  w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  w.Samples.wValidBitsPerSample = valid_bits;
  w.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  w.SubFormat = floating ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  std::wostringstream label;
  label << rate << L" Hz, " << (floating ? L"float" : L"PCM") << bits;
  if (!floating && valid_bits != bits) label << L"/" << valid_bits;
  result.label = label.str();
  return result;
}

std::vector<AudioFormat> CandidateFormats(std::uint32_t preferred_rate) {
  std::vector<std::uint32_t> rates{preferred_rate, 48000, 44100, 96000};
  std::sort(rates.begin(), rates.end());
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
  if (const auto it = std::find(rates.begin(), rates.end(), preferred_rate); it != rates.end()) {
    std::rotate(rates.begin(), it, it + 1);
  }
  std::vector<AudioFormat> formats;
  formats.reserve(rates.size() * 5);
  for (const auto rate : rates) {
    formats.push_back(MakeFormat(rate, 32, 32, true));
    formats.push_back(MakeFormat(rate, 32, 32, false));
    formats.push_back(MakeFormat(rate, 32, 24, false));
    formats.push_back(MakeFormat(rate, 24, 24, false));
    formats.push_back(MakeFormat(rate, 16, 16, false));
  }
  return formats;
}

REFERENCE_TIME FramesToReferenceTime(std::uint32_t frames, std::uint32_t rate) noexcept {
  return static_cast<REFERENCE_TIME>((10'000'000ULL * frames + rate / 2) / rate);
}

bool TryInitializeExact(IMMDevice* device, const AudioFormat& format,
                        std::uint32_t requested, std::uint32_t* actual,
                        REFERENCE_TIME* latency = nullptr) {
  ComPtr<IAudioClient> client;
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf()));
  if (FAILED(hr)) return false;
  const auto period = FramesToReferenceTime(requested, format.wave.Format.nSamplesPerSec);
  hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                          period, period,
                          reinterpret_cast<const WAVEFORMATEX*>(&format.wave), nullptr);
  UINT32 frames = 0;
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    if (FAILED(client->GetBufferSize(&frames))) return false;
    *actual = frames;
    return false;
  }
  if (FAILED(hr) || FAILED(client->GetBufferSize(&frames))) return false;
  *actual = frames;
  if (latency) client->GetStreamLatency(latency);
  return frames == requested;
}

inline float Clamp(float sample) noexcept {
  return std::clamp(sample, -1.0f, std::nextafter(1.0f, 0.0f));
}

}  // namespace

std::vector<EndpointInfo> EnumerateRenderEndpoints() {
  ComApartment apartment;
  if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
    ThrowHr("CoInitializeEx", apartment.result());
  }
  auto enumerator = CreateEnumerator();
  ComPtr<IMMDeviceCollection> collection;
  CheckHr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED |
    DEVICE_STATE_UNPLUGGED, &collection), "IMMDeviceEnumerator::EnumAudioEndpoints");
  UINT count = 0;
  CheckHr(collection->GetCount(&count), "IMMDeviceCollection::GetCount");
  std::vector<EndpointInfo> result;
  result.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device;
    CheckHr(collection->Item(i, &device), "IMMDeviceCollection::Item");
    LPWSTR raw_id = nullptr;
    CheckHr(device->GetId(&raw_id), "IMMDevice::GetId");
    DWORD state = 0;
    CheckHr(device->GetState(&state), "IMMDevice::GetState");
    result.push_back({raw_id, DeviceName(device.Get()), state});
    CoTaskMemFree(raw_id);
  }
  return result;
}

std::wstring ReadConfiguredEndpointId() {
  wchar_t value[1024]{};
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kEndpointValue, RRF_RT_REG_SZ,
                   nullptr, value, &bytes) != ERROR_SUCCESS) return {};
  return value;
}

bool WriteConfiguredEndpointId(const std::wstring& id) {
  HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  const auto bytes = static_cast<DWORD>((id.size() + 1) * sizeof(wchar_t));
  const auto status = RegSetValueExW(key, kEndpointValue, 0, REG_SZ,
    reinterpret_cast<const BYTE*>(id.c_str()), bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

std::wstring SettingsFilePath() {
  std::array<wchar_t, 32768> root{};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(),
                                                static_cast<DWORD>(root.size()));
  if (!length || length >= root.size()) return L"XtremeASIO.yaml";
  return std::wstring(root.data(), length) + L"\\XtremeASIO\\config.yaml";
}

bool WriteDriverSettings(const DriverSettings& settings) noexcept {
  if ((settings.sample_rate != 44100 && settings.sample_rate != 48000) ||
      (settings.preferred_buffer_size != 64 && settings.preferred_buffer_size != 128 &&
       settings.preferred_buffer_size != 256 && settings.preferred_buffer_size != 512) ||
      !std::isfinite(settings.jbl_gain_db) || settings.jbl_gain_db < -96.0 ||
      settings.jbl_gain_db > 0.0) return false;
  try {
    const std::filesystem::path path(SettingsFilePath());
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "# XtremeASIO settings. Close and reopen the ASIO device after editing.\n"
         << "# Supported buffer sizes: 64, 128, 256, 512.\n"
         << "# Supported host sample rates: 44100 and 48000; the JBL remains native 48000.\n"
         << "sample_rate: " << settings.sample_rate << '\n'
         << "preferred_buffer_size: " << settings.preferred_buffer_size << '\n'
         << std::fixed << std::setprecision(2)
         << "jbl_gain_db: " << settings.jbl_gain_db << '\n';
    return static_cast<bool>(file);
  } catch (...) {
    return false;
  }
}

DriverSettings ReadDriverSettings() noexcept {
  DriverSettings settings;
  try {
    std::ifstream file{std::filesystem::path(SettingsFilePath())};
    if (!file) {
      WriteDriverSettings(settings);
      return settings;
    }
    std::string line;
    while (std::getline(file, line)) {
      if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
      };
      const auto key = trim(line.substr(0, colon));
      const auto value = trim(line.substr(colon + 1));
      if (key == "sample_rate") settings.sample_rate = static_cast<std::uint32_t>(std::stoul(value));
      else if (key == "preferred_buffer_size") settings.preferred_buffer_size = static_cast<std::uint32_t>(std::stoul(value));
      else if (key == "jbl_gain_db") settings.jbl_gain_db = std::stod(value);
    }
  } catch (...) {
    return DriverSettings{};
  }
  if ((settings.sample_rate != 44100 && settings.sample_rate != 48000) ||
      (settings.preferred_buffer_size != 64 && settings.preferred_buffer_size != 128 &&
       settings.preferred_buffer_size != 256 && settings.preferred_buffer_size != 512) ||
      !std::isfinite(settings.jbl_gain_db) || settings.jbl_gain_db < -96.0 ||
      settings.jbl_gain_db > 0.0) return DriverSettings{};
  return settings;
}

double ReadOutputGainDb() noexcept {
  return ReadDriverSettings().jbl_gain_db;
}

bool WriteOutputGainDb(double gain_db) noexcept {
  if (!std::isfinite(gain_db) || gain_db < -96.0 || gain_db > 0.0) return false;
  auto settings = ReadDriverSettings();
  settings.jbl_gain_db = gain_db;
  return WriteDriverSettings(settings);
}
EndpointInfo SelectEndpoint(const std::wstring& requested) {
  const auto endpoints = EnumerateRenderEndpoints();
  const auto configured = requested.empty() ? ReadConfiguredEndpointId() : requested;
  if (!configured.empty()) {
    if (const auto it = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& item) {
          return item.id == configured;
        }); it != endpoints.end()) return *it;
  }
  if (const auto it = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& item) {
        return item.state == DEVICE_STATE_ACTIVE &&
          item.name.find(kTargetEndpointName) != std::wstring::npos;
      }); it != endpoints.end()) return *it;
  throw std::runtime_error("No active render endpoint containing 'Zgmicro AUDIO' was found");
}

EndpointProbe ProbeEndpoint(const EndpointInfo& endpoint, std::uint32_t preferred_rate) {
  ComApartment apartment;
  if (FAILED(apartment.result()) && apartment.result() != RPC_E_CHANGED_MODE) {
    ThrowHr("CoInitializeEx", apartment.result());
  }
  auto device = DeviceFromId(endpoint.id);
  EndpointProbe probe;
  probe.endpoint = endpoint;
  {
    ComPtr<IAudioClient> client;
    CheckHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
      reinterpret_cast<void**>(client.GetAddressOf())), "IMMDevice::Activate(IAudioClient)");
    CheckHr(client->GetDevicePeriod(&probe.default_period, &probe.minimum_period),
            "IAudioClient::GetDevicePeriod");
    for (auto& format : CandidateFormats(preferred_rate)) {
      if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
          reinterpret_cast<const WAVEFORMATEX*>(&format.wave), nullptr) == S_OK) {
        probe.formats.push_back(format);
      }
    }
  }
  if (!probe.formats.empty()) {
    const auto preferred = std::find_if(probe.formats.begin(), probe.formats.end(),
      [preferred_rate](const AudioFormat& f) { return f.wave.Format.nSamplesPerSec == preferred_rate; });
    const auto& format = preferred != probe.formats.end() ? *preferred : probe.formats.front();
    for (const auto frames : kCandidateBufferFrames) {
      std::uint32_t actual = 0;
      if (TryInitializeExact(device.Get(), format, frames, &actual)) {
        probe.supported_buffer_frames.push_back(frames);
      }
    }
  }
  return probe;
}

std::string HResultText(HRESULT hr) {
  char hex[16]{};
  std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));
  LPSTR raw = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const auto size = FormatMessageA(flags, nullptr, static_cast<DWORD>(hr), 0,
    reinterpret_cast<LPSTR>(&raw), 0, nullptr);
  std::string result(hex);
  if (size && raw) {
    result += " (";
    result.append(raw, size);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    result += ')';
  }
  if (raw) LocalFree(raw);
  return result;
}

double ReferenceTimeMs(REFERENCE_TIME value) noexcept {
  return static_cast<double>(value) / 10'000.0;
}

TelemetrySnapshot ReadTelemetry() noexcept {
  TelemetryMap map(false);
  if (!map.data) return {};
  return {
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->xruns, 0, 0)),
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->callbacks, 0, 0)),
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->wait_timeouts, 0, 0)),
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->io_errors, 0, 0)),
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->late_gaps, 0, 0)),
    static_cast<std::uint64_t>(InterlockedCompareExchange64(&map.data->concealed_frames, 0, 0)),
    static_cast<std::uint32_t>(InterlockedCompareExchange(&map.data->max_concealed_frames, 0, 0)),
    static_cast<std::uint32_t>(InterlockedCompareExchange(&map.data->sample_rate, 0, 0)),
    static_cast<std::uint32_t>(InterlockedCompareExchange(&map.data->buffer_frames, 0, 0)),
    static_cast<std::uint32_t>(InterlockedCompareExchange(&map.data->running, 0, 0))
  };
}

ExclusiveRenderer::ExclusiveRenderer() = default;
ExclusiveRenderer::~ExclusiveRenderer() { Close(); }

void ExclusiveRenderer::Open(const EndpointInfo& endpoint, const AudioFormat& format,
                             std::uint32_t requested_frames, FillCallback callback, void* context,
                             bool event_driven) {
  Close();
  if (!callback || requested_frames == 0) throw std::invalid_argument("Invalid render callback/buffer size");
  open_com_result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(open_com_result_) && open_com_result_ != RPC_E_CHANGED_MODE) {
    ThrowHr("CoInitializeEx", open_com_result_);
  }
  auto device = DeviceFromId(endpoint.id);
  CheckHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
    reinterpret_cast<void**>(audio_client_.GetAddressOf())), "IMMDevice::Activate(IAudioClient)");
  const auto period = FramesToReferenceTime(requested_frames, format.wave.Format.nSamplesPerSec);
  event_driven_ = event_driven;
  const auto flags = event_driven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
  const HRESULT init_hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
    flags, period, event_driven ? period : 0,
    reinterpret_cast<const WAVEFORMATEX*>(&format.wave), nullptr);
  if (FAILED(init_hr)) ThrowHr("IAudioClient::Initialize(exclusive/event)", init_hr);
  UINT32 actual = 0;
  CheckHr(audio_client_->GetBufferSize(&actual), "IAudioClient::GetBufferSize");
  if (actual != requested_frames && event_driven_) {
    Close();
    throw std::runtime_error("Endpoint aligned the requested buffer to a different frame count");
  }
  REFERENCE_TIME latency = 0;
  CheckHr(audio_client_->GetStreamLatency(&latency), "IAudioClient::GetStreamLatency");
  CheckHr(audio_client_->GetService(IID_PPV_ARGS(&render_client_)),
          "IAudioClient::GetService(IAudioRenderClient)");
  CheckHr(audio_client_->GetService(IID_PPV_ARGS(&audio_clock_)),
          "IAudioClient::GetService(IAudioClock)");
  audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  safety_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
  if (!safety_timer_) safety_timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (!audio_event_ || !stop_event_ || !ready_event_ || !safety_timer_) {
    Close();
    ThrowHr("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
  }
  if (event_driven_) {
    CheckHr(audio_client_->SetEventHandle(audio_event_), "IAudioClient::SetEventHandle");
  }
  format_ = format;
  callback_ = callback;
  callback_context_ = context;
  buffer_frames_ = actual;
  callback_frames_ = requested_frames;
  latency_frames_ = static_cast<std::uint32_t>(
    (static_cast<std::uint64_t>(latency) * format.wave.Format.nSamplesPerSec + 9'999'999) / 10'000'000);
  scratch_.assign(static_cast<std::size_t>(actual) * 2, 0.0f);
  xruns_.store(0, std::memory_order_relaxed);
  callbacks_.store(0, std::memory_order_relaxed);
  telemetry_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
    static_cast<DWORD>(sizeof(SharedTelemetry)), kTelemetryName);
  if (telemetry_mapping_) {
    telemetry_view_ = MapViewOfFile(telemetry_mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                    sizeof(SharedTelemetry));
  }
}

void ExclusiveRenderer::Start() {
  if (!IsOpen() || thread_.joinable()) throw std::logic_error("Renderer is not open or already started");
  ResetEvent(stop_event_);
  ResetEvent(ready_event_);
  thread_start_result_ = E_FAIL;
  thread_ = std::thread([this] { ThreadMain(); });
  if (WaitForSingleObject(ready_event_, 5000) != WAIT_OBJECT_0 || FAILED(thread_start_result_)) {
    Stop();
    ThrowHr("Realtime render thread startup", thread_start_result_);
  }
}

void ExclusiveRenderer::Stop() noexcept {
  if (stop_event_) SetEvent(stop_event_);
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_release);
}

void ExclusiveRenderer::Close() noexcept {
  Stop();
  render_client_.Reset();
  audio_clock_.Reset();
  audio_client_.Reset();
  if (audio_event_) CloseHandle(audio_event_);
  if (stop_event_) CloseHandle(stop_event_);
  if (ready_event_) CloseHandle(ready_event_);
  if (safety_timer_) CloseHandle(safety_timer_);
  if (telemetry_view_) UnmapViewOfFile(telemetry_view_);
  if (telemetry_mapping_) CloseHandle(telemetry_mapping_);
  audio_event_ = stop_event_ = ready_event_ = nullptr;
  safety_timer_ = nullptr;
  telemetry_mapping_ = nullptr;
  telemetry_view_ = nullptr;
  scratch_.clear();
  buffer_frames_ = callback_frames_ = latency_frames_ = 0;
  callback_ = nullptr;
  callback_context_ = nullptr;
  if (SUCCEEDED(open_com_result_)) CoUninitialize();
  open_com_result_ = E_FAIL;
}

void ExclusiveRenderer::ThreadMain() noexcept {
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
    InterlockedExchange(&telemetry->sample_rate, static_cast<LONG>(SampleRate()));
    InterlockedExchange(&telemetry->buffer_frames, static_cast<LONG>(callback_frames_));
    InterlockedExchange(&telemetry->running, 0);
  }

  thread_start_result_ = S_OK;
  BYTE* initial = nullptr;
  thread_start_result_ = render_client_->GetBuffer(buffer_frames_, &initial);
  if (SUCCEEDED(thread_start_result_)) {
    thread_start_result_ = render_client_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
  }
  if (SUCCEEDED(thread_start_result_)) thread_start_result_ = audio_client_->Start();
  if (SUCCEEDED(thread_start_result_)) {
    running_.store(true, std::memory_order_release);
    if (telemetry) InterlockedExchange(&telemetry->running, 1);
  }
  SetEvent(ready_event_);

  if (SUCCEEDED(thread_start_result_) && !event_driven_) {
    LARGE_INTEGER due{};
    due.QuadPart = -10'000;
    SetWaitableTimerEx(safety_timer_, &due, 1, nullptr, nullptr, nullptr, 0);
    const HANDLE waits[]{stop_event_, safety_timer_};
    while (true) {
      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0) break;
      if (wait != WAIT_OBJECT_0 + 1) {
        CountXrun();
        continue;
      }
      UINT32 padding = 0;
      if (FAILED(audio_client_->GetCurrentPadding(&padding)) || padding > buffer_frames_) {
        CountXrun();
        continue;
      }
      const auto available = buffer_frames_ - padding;
      if (available < callback_frames_) continue;
      if (available >= callback_frames_ * 2) CountXrun();
      if (FillEndpointBuffer(callback_frames_, false) && telemetry) {
        InterlockedIncrement64(&telemetry->callbacks);
      }
    }
    CancelWaitableTimer(safety_timer_);
  } else if (SUCCEEDED(thread_start_result_)) {
    const HANDLE waits[]{stop_event_, audio_event_};
    while (true) {
      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 10);
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_OBJECT_0 + 1) {
        if (FillEndpointBuffer(callback_frames_, false) && telemetry) {
          InterlockedIncrement64(&telemetry->callbacks);
        }
      } else if (wait == WAIT_TIMEOUT) {
        CountXrun();
      } else {
        CountXrun();
      }
    }
  }  if (SUCCEEDED(thread_start_result_)) {
    audio_client_->Stop();
    audio_client_->Reset();
  }
  running_.store(false, std::memory_order_release);
  if (telemetry) InterlockedExchange(&telemetry->running, 0);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (SUCCEEDED(com_hr)) CoUninitialize();
}
bool ExclusiveRenderer::FillEndpointBuffer(std::uint32_t frames, bool priming) noexcept {
  BYTE* destination = nullptr;
  if (FAILED(render_client_->GetBuffer(frames, &destination))) {
    CountXrun();
    return false;
  }
  std::fill_n(scratch_.data(), static_cast<std::size_t>(frames) * 2, 0.0f);
  callback_(callback_context_, scratch_.data(), frames, priming);
  ConvertToDevice(destination, frames);
  if (!priming) callbacks_.fetch_add(1, std::memory_order_relaxed);
  if (FAILED(render_client_->ReleaseBuffer(frames, 0))) {
    CountXrun();
    return false;
  }
  return true;
}

void ExclusiveRenderer::ConvertToDevice(BYTE* destination, std::uint32_t frames) noexcept {
  const auto samples = static_cast<std::size_t>(frames) * 2;
  const bool floating = format_.wave.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  const WORD bits = format_.wave.Format.wBitsPerSample;
  const WORD valid = format_.wave.Samples.wValidBitsPerSample;
  if (floating && bits == 32) {
    std::memcpy(destination, scratch_.data(), samples * sizeof(float));
    return;
  }
  if (bits == 16) {
    auto* out = reinterpret_cast<std::int16_t*>(destination);
    for (std::size_t i = 0; i < samples; ++i) out[i] = static_cast<std::int16_t>(std::lrintf(Clamp(scratch_[i]) * 32768.0f));
  } else if (bits == 24) {
    for (std::size_t i = 0; i < samples; ++i) {
      const auto value = static_cast<std::int32_t>(std::lrintf(Clamp(scratch_[i]) * 8388608.0f));
      destination[i * 3] = static_cast<BYTE>(value);
      destination[i * 3 + 1] = static_cast<BYTE>(value >> 8);
      destination[i * 3 + 2] = static_cast<BYTE>(value >> 16);
    }
  } else if (bits == 32) {
    auto* out = reinterpret_cast<std::int32_t*>(destination);
    const int shift = 32 - valid;
    const double scale = std::ldexp(1.0, valid - 1);
    for (std::size_t i = 0; i < samples; ++i) {
      const auto value = static_cast<std::int64_t>(std::llround(static_cast<double>(Clamp(scratch_[i])) * scale));
      out[i] = static_cast<std::int32_t>(value << shift);
    }
  } else {
    std::memset(destination, 0, static_cast<std::size_t>(frames) * format_.wave.Format.nBlockAlign);
  }
}

void ExclusiveRenderer::CountXrun() noexcept {
  xruns_.fetch_add(1, std::memory_order_relaxed);
  if (auto* telemetry = static_cast<SharedTelemetry*>(telemetry_view_)) {
    InterlockedIncrement64(&telemetry->xruns);
  }
}

}  // namespace xtreme

