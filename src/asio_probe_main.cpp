#include <windows.h>
#include <objbase.h>
#include <iostream>
#include <array>
#include <atomic>
#include <algorithm>
#include "iasiodrv.h"
namespace {
std::array<ASIOBufferInfo, 6> g_buffers{};
std::atomic<unsigned long long> g_callbacks{0};
long g_frames = 64;
void Switch(long index, ASIOBool) noexcept {
  for (long i = 2; i < 6; ++i) {
    std::fill_n(static_cast<std::int32_t*>(g_buffers[i].buffers[index]), g_frames, 0);
  }
  g_callbacks.fetch_add(1, std::memory_order_relaxed);
}
void RateChanged(ASIOSampleRate) {}
long Message(long, long, void*, double*) { return 0; }
ASIOTime* TimeInfo(ASIOTime*, long index, ASIOBool direct) { Switch(index, direct); return nullptr; }
constexpr CLSID kUmcClsid{0x0351302f, 0xb1f1, 0x4a5d,
  {0x86, 0x13, 0x78, 0x7f, 0x77, 0xc2, 0x0e, 0xa4}};
}
int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IASIO* driver = nullptr;
  HMODULE vendor = LoadLibraryW(L"c:\\program files\\behringer\\umc_audio_driver\\x64\\umc_audioasio_x64.dll");
  using GetClassObject = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
  auto get_class = vendor ? reinterpret_cast<GetClassObject>(GetProcAddress(vendor, "DllGetClassObject")) : nullptr;
  IClassFactory* factory = nullptr;
  HRESULT created = get_class ? get_class(kUmcClsid, IID_IClassFactory,
    reinterpret_cast<void**>(&factory)) : HRESULT_FROM_WIN32(GetLastError());
  if (SUCCEEDED(created)) {
    created = factory->CreateInstance(nullptr, kUmcClsid, reinterpret_cast<void**>(&driver));
    factory->Release();
  }
  if (FAILED(created) || !driver) {
    std::cerr << "CoCreateInstance failed: 0x" << std::hex
              << static_cast<unsigned long>(created) << '\n';
    if (SUCCEEDED(com)) CoUninitialize();
    return 1;
  }
  char name[32]{};
  char error[124]{};
  driver->getDriverName(name);
  std::cout << "name=" << name << " version=" << driver->getDriverVersion() << '\n';
  if (driver->init(GetDesktopWindow()) != ASIOTrue) {
    driver->getErrorMessage(error);
    std::cerr << "init failed: " << error << '\n';
    driver->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return 2;
  }
  long inputs = 0, outputs = 0;
  long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
  ASIOSampleRate rate = 0;
  driver->getChannels(&inputs, &outputs);
  driver->getBufferSize(&minimum, &maximum, &preferred, &granularity);
  driver->getSampleRate(&rate);
  std::cout << "channels=" << inputs << " in / " << outputs << " out\n"
            << "buffers=" << minimum << ".." << maximum
            << " preferred=" << preferred << " granularity=" << granularity << '\n'
            << "sample_rate=" << rate
            << " can_48000=" << driver->canSampleRate(48000.0) << '\n';
  for (long input = 0; input < inputs; ++input) {
    ASIOChannelInfo info{};
    info.channel = input;
    info.isInput = ASIOTrue;
    const auto status = driver->getChannelInfo(&info);
    std::cout << "input " << input << " status=" << status << " type=" << info.type
              << " name=" << info.name << '\n';
  }
  for (long output = 0; output < outputs; ++output) {
    ASIOChannelInfo info{};
    info.channel = output;
    info.isInput = ASIOFalse;
    const auto status = driver->getChannelInfo(&info);
    std::cout << "output " << output << " status=" << status << " type=" << info.type
              << " name=" << info.name << '\n';
  }
  for (long i = 0; i < 2; ++i) {
    g_buffers[i].isInput = ASIOTrue;
    g_buffers[i].channelNum = i;
  }
  for (long i = 0; i < 4; ++i) {
    g_buffers[i + 2].isInput = ASIOFalse;
    g_buffers[i + 2].channelNum = i;
  }
  ASIOCallbacks callbacks{&Switch, &RateChanged, &Message, &TimeInfo};
  const auto created_buffers = driver->createBuffers(g_buffers.data(), 6, g_frames, &callbacks);
  std::cout << "create_64=" << created_buffers << '\n';
  if (created_buffers == ASE_OK) {
    const auto started = driver->start();
    std::cout << "start=" << started << '\n';
    if (started == ASE_OK) {
      Sleep(10000);
      driver->stop();
      long in_latency = 0, out_latency = 0;
      driver->getLatencies(&in_latency, &out_latency);
      std::cout << "callbacks_10s=" << g_callbacks.load()
                << " input_latency=" << in_latency
                << " output_latency=" << out_latency << '\n';
    }
    driver->disposeBuffers();
  }
  driver->Release();
  if (vendor) FreeLibrary(vendor);
  if (SUCCEEDED(com)) CoUninitialize();
  return 0;
}