#include "xtreme/wasapi.hpp"
#include "xtreme/ks.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "iasiodrv.h"

namespace {

// {2D926C13-F819-4F32-BD5D-8148E85913AB}
constexpr CLSID kDriverClsid{0x2d926c13, 0xf819, 0x4f32,
  {0xbd, 0x5d, 0x81, 0x48, 0xe8, 0x59, 0x13, 0xab}};
constexpr wchar_t kDriverName[] = L"XtremeASIO JBL USB";
constexpr wchar_t kAsioRegistryPath[] = L"SOFTWARE\\ASIO\\XtremeASIO JBL USB";

HMODULE g_module{};
std::atomic<long> g_object_count{0};
std::atomic<long> g_server_locks{0};

void CopyText(char* destination, std::size_t capacity, const char* source) noexcept {
  if (!destination || capacity == 0) return;
  strncpy_s(destination, capacity, source, _TRUNCATE);
}

template <typename T>
void SetAsio64(T& destination, std::uint64_t value) noexcept {
  destination.hi = static_cast<unsigned long>(value >> 32);
  destination.lo = static_cast<unsigned long>(value & 0xffffffffULL);
}
bool SetRegistryString(HKEY root, const std::wstring& path, const wchar_t* name,
                       const std::wstring& value) {
  HKEY key{};
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS) return false;
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const auto status = RegSetValueExW(key, name, 0, REG_SZ,
    reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

class XtremeAsio final : public IASIO {
public:
  XtremeAsio() { g_object_count.fetch_add(1, std::memory_order_relaxed); }
  ~XtremeAsio() {
    disposeBuffers();
    g_object_count.fetch_sub(1, std::memory_order_relaxed);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == IID_IUnknown || iid == kDriverClsid) {
      *object = static_cast<IASIO*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1) + 1; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = references_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }

  ASIOBool init(void*) override {
    try {
      (void)xtreme::SelectEndpoint();
      const auto gain_db = xtreme::ReadOutputGainDb();
      output_gain_ = static_cast<float>(std::pow(10.0, gain_db / 20.0));
      initialized_ = true;
      SetError("");
      return ASIOTrue;
    } catch (const std::exception& error) {
      SetError(error.what());
      return ASIOFalse;
    }
  }

  void getDriverName(char* name) override { CopyText(name, 32, "XtremeASIO JBL USB"); }
  long getDriverVersion() override { return 2; }
  void getErrorMessage(char* message) override { CopyText(message, 124, error_.data()); }

  ASIOError start() override {
    if (!buffers_created_ || running_) return ASE_InvalidMode;
    try {
      sample_position_.store(0, std::memory_order_relaxed);
      next_play_buffer_ = 0;
      renderer_.Start();
      running_ = true;
      return ASE_OK;
    } catch (const std::exception& error) {
      SetError(error.what());
      return ASE_HWMalfunction;
    }
  }

  ASIOError stop() override {
    if (running_) renderer_.Stop();
    running_ = false;
    return ASE_OK;
  }

  ASIOError getChannels(long* inputs, long* outputs) override {
    if (!inputs || !outputs) return ASE_InvalidParameter;
    *inputs = 0;
    *outputs = 2;
    return initialized_ ? ASE_OK : ASE_NotPresent;
  }

  ASIOError getLatencies(long* input, long* output) override {
    if (!input || !output) return ASE_InvalidParameter;
    *input = 0;
    const auto endpoint_latency = renderer_.IsOpen() ? renderer_.StreamLatencyFrames() : buffer_size_;
    *output = static_cast<long>(endpoint_latency);
    return buffers_created_ ? ASE_OK : ASE_InvalidMode;
  }

  ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override {
    if (!minimum || !maximum || !preferred || !granularity) return ASE_InvalidParameter;
    if (!initialized_) return ASE_NotPresent;
    *minimum = 96;
    *maximum = 96;
    *preferred = 96;
    *granularity = 0;
    return ASE_OK;
  }

  ASIOError canSampleRate(ASIOSampleRate rate) override {
    return initialized_ && std::abs(rate - 48000.0) < 0.5 ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    if (!rate) return ASE_InvalidParameter;
    *rate = 48000.0;
    return initialized_ ? ASE_OK : ASE_NoClock;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    return canSampleRate(rate);
  }

  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
    if (!clocks || !count || *count < 1) return ASE_InvalidParameter;
    clocks[0] = {};
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    CopyText(clocks[0].name, sizeof(clocks[0].name), "Zgmicro USB clock");
    *count = 1;
    return ASE_OK;
  }
  ASIOError setClockSource(long reference) override { return reference == 0 ? ASE_OK : ASE_InvalidParameter; }

  ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* timestamp) override {
    if (!samples || !timestamp) return ASE_InvalidParameter;
    SetAsio64(*samples, sample_position_.load(std::memory_order_relaxed));
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    SetAsio64(*timestamp, QpcNanoseconds(now.QuadPart));
    return running_ ? ASE_OK : ASE_SPNotAdvancing;
  }

  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (!info || info->isInput == ASIOTrue || info->channel < 0 || info->channel > 1) {
      return ASE_InvalidParameter;
    }
    info->isActive = active_[info->channel] ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB;
    CopyText(info->name, sizeof(info->name), info->channel == 0 ? "Zgmicro Out L" : "Zgmicro Out R");
    return ASE_OK;
  }

  ASIOError createBuffers(ASIOBufferInfo* infos, long channel_count, long frames,
                          ASIOCallbacks* callbacks) override {
    if (!initialized_ || buffers_created_ || !infos || !callbacks ||
        channel_count < 1 || channel_count > 2 || frames <= 0) return ASE_InvalidMode;
    if (frames != 96) return ASE_InvalidMode;

    std::array<bool, 2> requested{};
    for (long i = 0; i < channel_count; ++i) {
      if (infos[i].isInput == ASIOTrue || infos[i].channelNum < 0 || infos[i].channelNum > 1 ||
          requested[infos[i].channelNum]) return ASE_InvalidParameter;
      requested[infos[i].channelNum] = true;
    }
    try {
      for (long channel = 0; channel < 2; ++channel) {
        if (!requested[channel]) continue;
        for (auto& buffer : buffers_[channel]) {
          buffer = std::make_unique<float[]>(static_cast<std::size_t>(frames));
          std::fill_n(buffer.get(), frames, 0.0f);
        }
      }
      for (long i = 0; i < channel_count; ++i) {
        const auto channel = infos[i].channelNum;
        infos[i].buffers[0] = buffers_[channel][0].get();
        infos[i].buffers[1] = buffers_[channel][1].get();
      }
      callbacks_ = callbacks;
      active_ = requested;
      buffer_size_ = static_cast<std::uint32_t>(frames);
      LARGE_INTEGER frequency{};
      QueryPerformanceFrequency(&frequency);
      qpc_frequency_ = frequency.QuadPart;
      time_info_mode_ = callbacks_->asioMessage &&
        callbacks_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
      renderer_.Open(buffer_size_, &XtremeAsio::RenderThunk, this);
      buffers_created_ = true;
      return ASE_OK;
    } catch (const std::bad_alloc&) {
      disposeBuffers();
      return ASE_NoMemory;
    } catch (const std::exception& error) {
      SetError(error.what());
      disposeBuffers();
      return ASE_HWMalfunction;
    }
  }

  ASIOError disposeBuffers() override {
    stop();
    renderer_.Close();
    for (auto& channel : buffers_) for (auto& buffer : channel) buffer.reset();
    active_ = {};
    callbacks_ = nullptr;
    buffers_created_ = false;
    buffer_size_ = 0;
    return ASE_OK;
  }

  ASIOError controlPanel() override {
    wchar_t module[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, module, MAX_PATH)) return ASE_NotPresent;
    std::wstring path(module);
    const auto slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"xtremeasio_diag.exe";
    std::wstring command = L"\"" + path + L"\" probe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) return ASE_NotPresent;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ASE_OK;
  }

  ASIOError future(long selector, void*) override {
    if (selector == kAsioCanTimeInfo || selector == kAsioCanReportOverload) return ASE_SUCCESS;
    return ASE_InvalidParameter;
  }
  ASIOError outputReady() override { return ASE_NotPresent; }

private:
  static void RenderThunk(void* context, float* destination, std::uint32_t frames,
                          bool priming) noexcept {
    static_cast<XtremeAsio*>(context)->Render(destination, frames, priming);
  }

  void Render(float* destination, std::uint32_t frames, bool) noexcept {
    if (!callbacks_ || frames != buffer_size_) {
      std::fill_n(destination, static_cast<std::size_t>(frames) * 2, 0.0f);
      return;
    }
    const auto play = next_play_buffer_;
    const auto position = sample_position_.load(std::memory_order_relaxed);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (time_info_mode_ && callbacks_->bufferSwitchTimeInfo) {
      asio_time_ = {};
      asio_time_.timeInfo.speed = 1.0;
      asio_time_.timeInfo.sampleRate = 48000.0;
      SetAsio64(asio_time_.timeInfo.samplePosition, position);
      SetAsio64(asio_time_.timeInfo.systemTime, QpcNanoseconds(now.QuadPart));
      asio_time_.timeInfo.flags = kSystemTimeValid | kSamplePositionValid |
                                  kSampleRateValid | kSpeedValid;
      callbacks_->bufferSwitchTimeInfo(&asio_time_, play, ASIOTrue);
    } else if (callbacks_->bufferSwitch) {
      callbacks_->bufferSwitch(play, ASIOTrue);
    }
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      destination[frame * 2] = active_[0] ? buffers_[0][play][frame] * output_gain_ : 0.0f;
      destination[frame * 2 + 1] = active_[1] ? buffers_[1][play][frame] * output_gain_ : 0.0f;
    }
    next_play_buffer_ = play ^ 1;
    sample_position_.store(position + frames, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t QpcNanoseconds(LONGLONG counter) const noexcept {
    if (qpc_frequency_ <= 0) return 0;
    const auto seconds = counter / qpc_frequency_;
    const auto remainder = counter % qpc_frequency_;
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL +
      static_cast<std::uint64_t>((remainder * 1'000'000'000LL) / qpc_frequency_);
  }

  void SetError(const char* error) noexcept { CopyText(error_.data(), error_.size(), error); }

  std::atomic<ULONG> references_{1};
  xtreme::KsRenderer renderer_;
  std::array<std::array<std::unique_ptr<float[]>, 2>, 2> buffers_{};
  std::array<bool, 2> active_{};
  ASIOCallbacks* callbacks_{};
  ASIOTime asio_time_{};
  std::array<char, 124> error_{};
  std::atomic<std::uint64_t> sample_position_{0};
  LONGLONG qpc_frequency_{};
  std::uint32_t buffer_size_{};
  long next_play_buffer_{1};
  bool initialized_{};
  bool buffers_created_{};
  bool running_{};
  bool time_info_mode_{};
  float output_gain_{0.12589254f};
};

class DriverFactory final : public IClassFactory {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1) + 1; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = references_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* driver = new (std::nothrow) XtremeAsio();
    if (!driver) return E_OUTOFMEMORY;
    const auto result = driver->QueryInterface(iid, object);
    driver->Release();
    return result;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock) g_server_locks.fetch_add(1, std::memory_order_relaxed);
    else g_server_locks.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
  }
private:
  std::atomic<ULONG> references_{1};
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
  if (clsid != kDriverClsid) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) DriverFactory();
  if (!factory) return E_OUTOFMEMORY;
  const auto result = factory->QueryInterface(iid, object);
  factory->Release();
  return result;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return g_object_count.load(std::memory_order_relaxed) == 0 &&
         g_server_locks.load(std::memory_order_relaxed) == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
  wchar_t module[MAX_PATH]{};
  if (!GetModuleFileNameW(g_module, module, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
  LPOLESTR clsid_raw{};
  if (FAILED(StringFromCLSID(kDriverClsid, &clsid_raw))) return E_FAIL;
  const std::wstring clsid(clsid_raw);
  CoTaskMemFree(clsid_raw);
  const std::wstring class_path = L"CLSID\\" + clsid;
  if (!SetRegistryString(HKEY_CLASSES_ROOT, class_path, nullptr, kDriverName) ||
      !SetRegistryString(HKEY_CLASSES_ROOT, class_path + L"\\InprocServer32", nullptr, module) ||
      !SetRegistryString(HKEY_CLASSES_ROOT, class_path + L"\\InprocServer32", L"ThreadingModel", L"Both") ||
      !SetRegistryString(HKEY_LOCAL_MACHINE, kAsioRegistryPath, L"CLSID", clsid) ||
      !SetRegistryString(HKEY_LOCAL_MACHINE, kAsioRegistryPath, L"Description", kDriverName)) {
    return HRESULT_FROM_WIN32(GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : GetLastError());
  }
  return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
  LPOLESTR clsid_raw{};
  if (FAILED(StringFromCLSID(kDriverClsid, &clsid_raw))) return E_FAIL;
  const std::wstring class_path = L"CLSID\\" + std::wstring(clsid_raw);
  CoTaskMemFree(clsid_raw);
  RegDeleteTreeW(HKEY_CLASSES_ROOT, class_path.c_str());
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAsioRegistryPath);
  return S_OK;
}

