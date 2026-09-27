#include <windows.h>
#include "xtreme/wasapi.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "iasiodrv.h"
#include "xtreme/ks.hpp"

namespace {
constexpr CLSID kDriverClsid{0x2d926c13,0xf819,0x4f32,{0xbd,0x5d,0x81,0x48,0xe8,0x59,0x13,0xab}};
constexpr CLSID kUmcClsid{0x0351302f,0xb1f1,0x4a5d,{0x86,0x13,0x78,0x7f,0x77,0xc2,0x0e,0xa4}};
constexpr wchar_t kDriverName[]=L"XtremeASIO UMC + JBL";
constexpr wchar_t kAsioRegistryPath[]=L"SOFTWARE\\ASIO\\XtremeASIO JBL USB";
constexpr std::uint32_t kDefaultFrames=64,kJblRate=48000,kJblFrames=96;
HMODULE g_module{}; std::atomic<long> g_objects{0},g_locks{0};

void CopyText(char* dst,std::size_t n,const char* src) noexcept {if(dst&&n) strncpy_s(dst,n,src,_TRUNCATE);}
bool SetReg(HKEY root,const std::wstring& path,const wchar_t* name,const std::wstring& value){
 HKEY key{}; if(RegCreateKeyExW(root,path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
 auto bytes=static_cast<DWORD>((value.size()+1)*sizeof(wchar_t));
 auto status=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),bytes); RegCloseKey(key); return status==ERROR_SUCCESS;
}

class StereoBridge {
public:
 void Allocate(){data_.assign(kCapacity*2,0.0f);}
 void Reset(std::uint32_t target_frames,std::uint32_t source_rate,std::uint32_t output_rate) noexcept {target_frames_=target_frames;nominal_ratio_=static_cast<double>(source_rate)/static_cast<double>(output_rate);std::fill(data_.begin(),data_.end(),0.0f);read_pos_=0;read_.store(0);write_.store(2);underflows_.store(0);overflows_.store(0);last_l_=last_r_=recovery_l_=recovery_r_=0.0f;recovery_remaining_=0;}
 bool Push(const float* l,const float* r,std::uint32_t frames,float gain) noexcept {
  auto w=write_.load(std::memory_order_relaxed);auto rd=read_.load(std::memory_order_acquire);
  if(w+frames-rd>=kCapacity){overflows_.fetch_add(1);return false;}
  for(std::uint32_t f=0;f<frames;++f){auto s=static_cast<std::size_t>(w+f)&(kCapacity-1);data_[s*2]=l[f]*gain;data_[s*2+1]=r[f]*gain;}
  write_.store(w+frames,std::memory_order_release);return true;
 }
 [[nodiscard]] std::uint64_t AvailableFrames() const noexcept {auto w=write_.load(std::memory_order_acquire);auto r=read_.load(std::memory_order_acquire);return w>=r?w-r:0;}
 std::uint32_t Pull(float* dst,std::uint32_t frames) noexcept {
  const auto w=write_.load(std::memory_order_acquire);const auto base=static_cast<std::uint64_t>(read_pos_);
  const auto available=w>=base?w-base:0;const double ratio=nominal_ratio_+std::clamp((static_cast<double>(available)-static_cast<double>(target_frames_))*0.00002,-0.005,0.005);
  std::uint32_t concealed=0;
  for(std::uint32_t f=0;f<frames;++f){const auto i=static_cast<std::uint64_t>(read_pos_);
   if(i>=w){if(!recovery_remaining_){recovery_l_=last_l_;recovery_r_=last_r_;recovery_remaining_=kRecoveryFrames;}dst[f*2]=last_l_;dst[f*2+1]=last_r_;++concealed;continue;}
   const auto a=static_cast<std::size_t>(i)&(kCapacity-1);const float frac=static_cast<float>(read_pos_-static_cast<double>(i));
   float l=data_[a*2],r=data_[a*2+1];
   if(i+1<w){const auto b=static_cast<std::size_t>(i+1)&(kCapacity-1);l+=(data_[b*2]-l)*frac;r+=(data_[b*2+1]-r)*frac;}
   else if(frac>0.0f){if(!recovery_remaining_){recovery_l_=last_l_;recovery_r_=last_r_;recovery_remaining_=kRecoveryFrames;}dst[f*2]=last_l_;dst[f*2+1]=last_r_;++concealed;continue;}
   if(recovery_remaining_){const float alpha=static_cast<float>(kRecoveryFrames-recovery_remaining_+1)/static_cast<float>(kRecoveryFrames);l=recovery_l_+(l-recovery_l_)*alpha;r=recovery_r_+(r-recovery_r_)*alpha;--recovery_remaining_;}
   dst[f*2]=last_l_=l;dst[f*2+1]=last_r_=r;read_pos_=std::min(read_pos_+ratio,static_cast<double>(w));}
  read_.store(static_cast<std::uint64_t>(read_pos_),std::memory_order_release);
  if(concealed)underflows_.fetch_add(1);return concealed;
 }
private:
 static constexpr std::uint64_t kCapacity=8192;
 static constexpr std::uint32_t kRecoveryFrames=16;
 std::vector<float> data_;std::atomic<std::uint64_t> write_{0},read_{0},underflows_{0},overflows_{0};double read_pos_{},nominal_ratio_{1.0};std::uint32_t target_frames_{176};float last_l_{},last_r_{},recovery_l_{},recovery_r_{};std::uint32_t recovery_remaining_{};
};

class Driver final:public IASIO{
public:
 Driver(){g_objects.fetch_add(1);} ~Driver(){disposeBuffers();if(umc_)umc_->Release();if(umc_module_)FreeLibrary(umc_module_);g_objects.fetch_sub(1);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==kDriverClsid){*out=static_cast<IASIO*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef() override{return refs_.fetch_add(1)+1;} ULONG STDMETHODCALLTYPE Release() override{auto n=refs_.fetch_sub(1)-1;if(!n)delete this;return n;}
 ASIOBool init(void* window) override{
  try{settings_=xtreme::ReadDriverSettings();sample_rate_=settings_.sample_rate;(void)xtreme::SelectEndpoint();wchar_t path[MAX_PATH]{};DWORD bytes=sizeof(path);
   constexpr wchar_t reg[]=L"CLSID\\{0351302F-B1F1-4A5D-8613-787F77C20EA4}\\InprocServer32";
   if(RegGetValueW(HKEY_CLASSES_ROOT,reg,nullptr,RRF_RT_REG_SZ,nullptr,path,&bytes)!=ERROR_SUCCESS){SetError("UMC ASIO driver is not registered");return ASIOFalse;}
   umc_module_=LoadLibraryW(path);if(!umc_module_){SetError("Could not load UMC ASIO DLL");return ASIOFalse;}
   using GetClass=HRESULT(__stdcall*)(REFCLSID,REFIID,void**);auto get=reinterpret_cast<GetClass>(GetProcAddress(umc_module_,"DllGetClassObject"));IClassFactory* factory{};
   if(!get||FAILED(get(kUmcClsid,IID_IClassFactory,reinterpret_cast<void**>(&factory)))){SetError("Could not get UMC ASIO factory");return ASIOFalse;}
   auto hr=factory->CreateInstance(nullptr,kUmcClsid,reinterpret_cast<void**>(&umc_));factory->Release();
   if(FAILED(hr)||!umc_||umc_->init(window)!=ASIOTrue){SetError("Could not initialize UMC ASIO driver");return ASIOFalse;}
   long in=0,out=0;umc_->getChannels(&in,&out);if(in<2||out<4||umc_->canSampleRate(sample_rate_)!=ASE_OK){SetError("UMC 2x4 at the configured sample rate is unavailable");return ASIOFalse;}
   for(long c=0;c<2;++c){ASIOChannelInfo x{};x.channel=c;x.isInput=ASIOTrue;if(umc_->getChannelInfo(&x)!=ASE_OK||x.type!=ASIOSTInt32LSB){SetError("Unsupported UMC input format");return ASIOFalse;}}
   for(long c=0;c<4;++c){ASIOChannelInfo x{};x.channel=c;x.isInput=ASIOFalse;if(umc_->getChannelInfo(&x)!=ASE_OK||x.type!=ASIOSTInt32LSB){SetError("Unsupported UMC output format");return ASIOFalse;}}
   if(umc_->setSampleRate(sample_rate_)!=ASE_OK){SetError("Could not set the configured UMC sample rate");return ASIOFalse;}bridge_.Allocate();gain_=static_cast<float>(std::pow(10.0,settings_.jbl_gain_db/20.0));initialized_=true;SetError("");return ASIOTrue;
  }catch(const std::exception& e){SetError(e.what());return ASIOFalse;}
 }
 void getDriverName(char* n) override{CopyText(n,32,"Xtreme UMC + JBL");} long getDriverVersion() override{return 4;} void getErrorMessage(char* m) override{CopyText(m,124,error_.data());}
 ASIOError start() override{if(!created_||running_)return ASE_InvalidMode;const auto target=BridgeTarget();bridge_.Reset(target,sample_rate_,kJblRate);running_=true;auto e=umc_->start();if(e!=ASE_OK){running_=false;return e;}for(int i=0;i<100&&bridge_.AvailableFrames()<target;++i)Sleep(1);try{jbl_.Start();return ASE_OK;}catch(const std::exception& ex){jbl_.Stop();umc_->stop();running_=false;SetError(ex.what());return ASE_HWMalfunction;}}
 ASIOError stop() override{jbl_.Stop();if(running_&&umc_)umc_->stop();running_=false;return ASE_OK;}
 ASIOError getChannels(long* in,long* out) override{if(!in||!out)return ASE_InvalidParameter;*in=2;*out=6;return initialized_?ASE_OK:ASE_NotPresent;}
 ASIOError getLatencies(long* in,long* out) override{if(!in||!out||!created_)return ASE_InvalidMode;long ui=0,uo=0;umc_->getLatencies(&ui,&uo);*in=ui;*out=std::max<long>(uo,static_cast<long>(BridgeTarget()+JblLatencyFrames()));return ASE_OK;}
 ASIOError getBufferSize(long* min,long* max,long* preferred,long* gran) override{if(!min||!max||!preferred||!gran)return ASE_InvalidParameter;*min=64;*max=512;*preferred=static_cast<long>(settings_.preferred_buffer_size);*gran=-1;return initialized_?ASE_OK:ASE_NotPresent;}
 ASIOError canSampleRate(ASIOSampleRate r) override{return initialized_&&std::abs(r-static_cast<double>(sample_rate_))<0.5?ASE_OK:ASE_NoClock;} ASIOError getSampleRate(ASIOSampleRate* r) override{if(!r)return ASE_InvalidParameter;*r=sample_rate_;return initialized_?ASE_OK:ASE_NoClock;} ASIOError setSampleRate(ASIOSampleRate r) override{return canSampleRate(r)==ASE_OK?umc_->setSampleRate(r):ASE_NoClock;}
 ASIOError getClockSources(ASIOClockSource* c,long* n) override{return umc_?umc_->getClockSources(c,n):ASE_NotPresent;} ASIOError setClockSource(long r) override{return umc_?umc_->setClockSource(r):ASE_NotPresent;} ASIOError getSamplePosition(ASIOSamples* s,ASIOTimeStamp* t) override{return umc_?umc_->getSamplePosition(s,t):ASE_NotPresent;}
 ASIOError getChannelInfo(ASIOChannelInfo* x) override{if(!x)return ASE_InvalidParameter;if(x->isInput==ASIOTrue){if(x->channel<0||x->channel>=2)return ASE_InvalidParameter;x->isActive=in_active_[x->channel]?ASIOTrue:ASIOFalse;x->channelGroup=0;x->type=ASIOSTFloat32LSB;CopyText(x->name,sizeof(x->name),x->channel?"UMC In 2":"UMC In 1");}else{if(x->channel<0||x->channel>=6)return ASE_InvalidParameter;static constexpr const char* names[]{"UMC Out 1","UMC Out 2","UMC Out 3","UMC Out 4","JBL Out L","JBL Out R"};x->isActive=out_active_[x->channel]?ASIOTrue:ASIOFalse;x->channelGroup=x->channel<4?0:1;x->type=ASIOSTFloat32LSB;CopyText(x->name,sizeof(x->name),names[x->channel]);}return initialized_?ASE_OK:ASE_NotPresent;}
 ASIOError createBuffers(ASIOBufferInfo* infos,long count,long frames,ASIOCallbacks* callbacks) override{
  if(!initialized_||created_||!infos||!callbacks||count<1||count>8||(frames!=64&&frames!=128&&frames!=256&&frames!=512))return ASE_InvalidMode;std::array<bool,2> ri{};std::array<bool,6> ro{};
  for(long i=0;i<count;++i){if(infos[i].isInput==ASIOTrue){if(infos[i].channelNum<0||infos[i].channelNum>=2||ri[infos[i].channelNum])return ASE_InvalidParameter;ri[infos[i].channelNum]=true;}else{if(infos[i].channelNum<0||infos[i].channelNum>=6||ro[infos[i].channelNum])return ASE_InvalidParameter;ro[infos[i].channelNum]=true;}}
  try{for(long c=0;c<2;++c)if(ri[c])for(auto& b:in_buf_[c]){b=std::make_unique<float[]>(frames);std::fill_n(b.get(),frames,0.0f);}for(long c=0;c<6;++c)if(ro[c])for(auto& b:out_buf_[c]){b=std::make_unique<float[]>(frames);std::fill_n(b.get(),frames,0.0f);}
   for(long i=0;i<count;++i){if(infos[i].isInput==ASIOTrue){infos[i].buffers[0]=in_buf_[infos[i].channelNum][0].get();infos[i].buffers[1]=in_buf_[infos[i].channelNum][1].get();}else{infos[i].buffers[0]=out_buf_[infos[i].channelNum][0].get();infos[i].buffers[1]=out_buf_[infos[i].channelNum][1].get();}}
   for(long c=0;c<2;++c){umc_buf_[c].isInput=ASIOTrue;umc_buf_[c].channelNum=c;}for(long c=0;c<4;++c){umc_buf_[c+2].isInput=ASIOFalse;umc_buf_[c+2].channelNum=c;}
   callbacks_=callbacks;in_active_=ri;out_active_=ro;host_frames_=static_cast<std::uint32_t>(frames);zero_.assign(host_frames_,0.0f);active_.store(this,std::memory_order_release);umc_cb_={&UmcSwitch,&UmcRate,&UmcMessage,&UmcTime};
   auto status=umc_->createBuffers(umc_buf_.data(),6,frames,&umc_cb_);if(status!=ASE_OK){Clear();return status;}umc_created_=true;jbl_.Open(kJblFrames,&JblThunk,this,kJblRate);created_=true;return ASE_OK;
  }catch(const std::bad_alloc&){jbl_.Close();if(umc_created_)umc_->disposeBuffers();umc_created_=false;Clear();return ASE_NoMemory;}catch(const std::exception& e){SetError(e.what());jbl_.Close();if(umc_created_)umc_->disposeBuffers();umc_created_=false;Clear();return ASE_HWMalfunction;}
 }
 ASIOError disposeBuffers() override{stop();jbl_.Close();if(umc_&&umc_created_)umc_->disposeBuffers();umc_created_=false;Clear();return ASE_OK;}
 ASIOError controlPanel() override{return umc_?umc_->controlPanel():ASE_NotPresent;} ASIOError future(long s,void*) override{return s==kAsioCanTimeInfo||s==kAsioCanReportOverload?ASE_SUCCESS:ASE_InvalidParameter;} ASIOError outputReady() override{return umc_?umc_->outputReady():ASE_NotPresent;}
private:
 static void UmcSwitch(long i,ASIOBool d) noexcept{if(auto* s=active_.load(std::memory_order_acquire))s->Process(i,d,nullptr);} static void UmcRate(ASIOSampleRate r) noexcept{if(auto*s=active_.load();s&&s->callbacks_&&s->callbacks_->sampleRateDidChange)s->callbacks_->sampleRateDidChange(r);} static long UmcMessage(long sel,long val,void* msg,double* opt) noexcept{if(sel==kAsioSupportsTimeInfo)return 1;if(auto*s=active_.load();s&&s->callbacks_&&s->callbacks_->asioMessage)return s->callbacks_->asioMessage(sel,val,msg,opt);return 0;} static ASIOTime* UmcTime(ASIOTime* t,long i,ASIOBool d) noexcept{if(auto*s=active_.load())s->Process(i,d,t);return t;} static void JblThunk(void* c,float* d,std::uint32_t f,bool) noexcept{auto* self=static_cast<Driver*>(c);if(auto n=self->bridge_.Pull(d,f))self->jbl_.ReportBridgeConcealment(n);}
 void Process(long index,ASIOBool direct,ASIOTime* time) noexcept{if(!callbacks_||index<0||index>1)return;constexpr float scale=1.0f/2147483648.0f;for(long c=0;c<2;++c)if(in_active_[c]){auto*src=static_cast<const std::int32_t*>(umc_buf_[c].buffers[index]);auto*dst=in_buf_[c][index].get();for(std::uint32_t f=0;f<host_frames_;++f)dst[f]=static_cast<float>(src[f])*scale;}
  if(time&&callbacks_->bufferSwitchTimeInfo)callbacks_->bufferSwitchTimeInfo(time,index,direct);else if(callbacks_->bufferSwitch)callbacks_->bufferSwitch(index,direct);
  for(long c=0;c<4;++c){auto*dst=static_cast<std::int32_t*>(umc_buf_[c+2].buffers[index]);auto*src=out_active_[c]?out_buf_[c][index].get():nullptr;for(std::uint32_t f=0;f<host_frames_;++f){float v=src?std::clamp(src[f],-1.0f,std::nextafter(1.0f,0.0f)):0.0f;dst[f]=static_cast<std::int32_t>(std::llround(v*2147483648.0));}}
  if(!bridge_.Push(out_active_[4]?out_buf_[4][index].get():zero_.data(),out_active_[5]?out_buf_[5][index].get():zero_.data(),host_frames_,gain_))jbl_.ReportExternalXrun();
 }
 [[nodiscard]] std::uint32_t JblLatencyFrames() const noexcept{return std::max<std::uint32_t>(1,(kJblFrames*sample_rate_+kJblRate/2)/kJblRate);}
 [[nodiscard]] std::uint32_t BridgeTarget() const noexcept{return host_frames_+std::max<std::uint32_t>(1,(sample_rate_*7+1499)/3000);}
 void Clear() noexcept{zero_.clear();host_frames_=kDefaultFrames;auto* expected=this;active_.compare_exchange_strong(expected,nullptr);for(auto&c:in_buf_)for(auto&b:c)b.reset();for(auto&c:out_buf_)for(auto&b:c)b.reset();in_active_={};out_active_={};callbacks_=nullptr;created_=false;}
 void SetError(const char* e) noexcept{CopyText(error_.data(),error_.size(),e);}
 static inline std::atomic<Driver*> active_{nullptr};std::atomic<ULONG> refs_{1};HMODULE umc_module_{};IASIO* umc_{};xtreme::KsRenderer jbl_;StereoBridge bridge_;std::array<ASIOBufferInfo,6> umc_buf_{};ASIOCallbacks umc_cb_{};ASIOCallbacks* callbacks_{};std::array<std::array<std::unique_ptr<float[]>,2>,2> in_buf_{};std::array<std::array<std::unique_ptr<float[]>,2>,6> out_buf_{};std::vector<float> zero_;xtreme::DriverSettings settings_{};std::uint32_t host_frames_{kDefaultFrames},sample_rate_{48000};std::array<bool,2> in_active_{};std::array<bool,6> out_active_{};std::array<char,124> error_{};float gain_{0.12589254f};bool initialized_{},created_{},umc_created_{},running_{};
};

class Factory final:public IClassFactory{public:HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i,void**o)override{if(!o)return E_POINTER;*o=nullptr;if(i==IID_IUnknown||i==IID_IClassFactory){*o=static_cast<IClassFactory*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}ULONG STDMETHODCALLTYPE AddRef()override{return refs_.fetch_add(1)+1;}ULONG STDMETHODCALLTYPE Release()override{auto n=refs_.fetch_sub(1)-1;if(!n)delete this;return n;}HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown*outer,REFIID i,void**o)override{if(outer)return CLASS_E_NOAGGREGATION;auto*d=new(std::nothrow)Driver();if(!d)return E_OUTOFMEMORY;auto r=d->QueryInterface(i,o);d->Release();return r;}HRESULT STDMETHODCALLTYPE LockServer(BOOL l)override{if(l)g_locks.fetch_add(1);else g_locks.fetch_sub(1);return S_OK;}private:std::atomic<ULONG>refs_{1};};
}

BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){if(r==DLL_PROCESS_ATTACH){g_module=h;DisableThreadLibraryCalls(h);}return TRUE;}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID c,REFIID i,void**o){if(c!=kDriverClsid)return CLASS_E_CLASSNOTAVAILABLE;auto*f=new(std::nothrow)Factory();if(!f)return E_OUTOFMEMORY;auto r=f->QueryInterface(i,o);f->Release();return r;}
extern "C" HRESULT __stdcall DllCanUnloadNow(){return g_objects.load()==0&&g_locks.load()==0?S_OK:S_FALSE;}
extern "C" HRESULT __stdcall DllRegisterServer(){wchar_t module[MAX_PATH]{};if(!GetModuleFileNameW(g_module,module,MAX_PATH))return HRESULT_FROM_WIN32(GetLastError());LPOLESTR raw{};if(FAILED(StringFromCLSID(kDriverClsid,&raw)))return E_FAIL;std::wstring clsid(raw);CoTaskMemFree(raw);std::wstring cp=L"CLSID\\"+clsid;if(!SetReg(HKEY_CLASSES_ROOT,cp,nullptr,kDriverName)||!SetReg(HKEY_CLASSES_ROOT,cp+L"\\InprocServer32",nullptr,module)||!SetReg(HKEY_CLASSES_ROOT,cp+L"\\InprocServer32",L"ThreadingModel",L"Both")||!SetReg(HKEY_LOCAL_MACHINE,kAsioRegistryPath,L"CLSID",clsid)||!SetReg(HKEY_LOCAL_MACHINE,kAsioRegistryPath,L"Description",kDriverName))return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);return S_OK;}
extern "C" HRESULT __stdcall DllUnregisterServer(){LPOLESTR raw{};if(FAILED(StringFromCLSID(kDriverClsid,&raw)))return E_FAIL;std::wstring cp=L"CLSID\\"+std::wstring(raw);CoTaskMemFree(raw);RegDeleteTreeW(HKEY_CLASSES_ROOT,cp.c_str());RegDeleteTreeW(HKEY_LOCAL_MACHINE,kAsioRegistryPath);return S_OK;}
