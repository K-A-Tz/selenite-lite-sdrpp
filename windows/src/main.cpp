#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>
#include <module.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <gui/smgui.h>
#include <utils/flog.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <condition_variable>

SDRPP_MOD_INFO{
    /* Name */ "selenite_source",
    /* Description */ "Selenite Lite USB Audio IQ and FT-817 CAT",
    /* Author */ "Community integration",
    /* Version */ 0, 1, 0,
    /* Max instances */ 1
};

namespace {
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.pop_back(); return s;
}
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.pop_back(); return w;
}
std::string err(const char* prefix, DWORD code = GetLastError()) { return std::string(prefix) + " (" + std::to_string(code) + ")"; }
struct DeviceEntry { std::wstring id; std::string name; };
std::vector<DeviceEntry> listInputs() {
    std::vector<DeviceEntry> result;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return result;
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll))) {
            UINT count = 0; coll->GetCount(&count);
            for (UINT i=0;i<count;i++) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                LPWSTR id = nullptr; IPropertyStore* props = nullptr;
                std::string label = "Audio input " + std::to_string(i);
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        label = narrow(pv.pwszVal);
                    PropVariantClear(&pv); props->Release();
                }
                if (SUCCEEDED(dev->GetId(&id))) { result.push_back({id, label}); CoTaskMemFree(id); }
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}
class Cat {
    HANDLE h = INVALID_HANDLE_VALUE;
public:
    ~Cat() { close(); }
    void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    bool open(const std::string& port) {
        close();
        std::string path = "\\\\.\\" + port;
        h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { flog::error("Selenite: CAT open failed {}", std::to_string(GetLastError())); return false; }
        DCB d{}; d.DCBlength = sizeof(d);
        if (!GetCommState(h, &d)) { close(); return false; }
        d.BaudRate = CBR_9600; d.ByteSize = 8; d.Parity = NOPARITY; d.StopBits = TWOSTOPBITS;
        d.fBinary = TRUE; d.fParity = FALSE; d.fOutxCtsFlow = FALSE; d.fOutxDsrFlow = FALSE;
        d.fDtrControl = DTR_CONTROL_DISABLE; d.fRtsControl = RTS_CONTROL_DISABLE;
        d.fOutX = FALSE; d.fInX = FALSE;
        if (!SetCommState(h, &d)) { flog::error("Selenite: CAT setup failed {}", std::to_string(GetLastError())); close(); return false; }
        COMMTIMEOUTS t{}; t.ReadIntervalTimeout = 50; t.ReadTotalTimeoutConstant = 250; t.WriteTotalTimeoutConstant = 500;
        SetCommTimeouts(h, &t); EscapeCommFunction(h, CLRDTR); EscapeCommFunction(h, CLRRTS);
        return true;
    }
    bool tune(uint64_t hz) {
        if (h == INVALID_HANDLE_VALUE || hz < 1500000ULL || hz > 32000000ULL) return false;
        uint64_t units = (hz + 5) / 10;
        BYTE cmd[5] = {};
        for (int i=3;i>=0;--i) { BYTE lo = units % 10; units /= 10; BYTE hi = units % 10; units /= 10; cmd[i] = (hi<<4)|lo; }
        cmd[4] = 0x01;
        char catPacket[64];
        std::snprintf(catPacket, sizeof(catPacket), "%02X %02X %02X %02X %02X",
                      static_cast<unsigned>(cmd[0]), static_cast<unsigned>(cmd[1]),
                      static_cast<unsigned>(cmd[2]), static_cast<unsigned>(cmd[3]),
                      static_cast<unsigned>(cmd[4]));
        flog::info("Selenite: CAT SET {} Hz, packet {}", std::to_string(hz), std::string(catPacket));
        // FT-817-compatible Selenite firmware requires 200 ms between bytes.
        // Keep the command bytes in order; do not pause after the final opcode.
        for (int i = 0; i < 5; ++i) {
            DWORD wrote = 0;
            if (!WriteFile(h, &cmd[i], 1, &wrote, nullptr) || wrote != 1) {
                flog::error("Selenite: CAT byte {} write failed", std::to_string(i + 1));
                return false;
            }
            if (i != 4) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return true;
    }
};
}

class SeleniteSource : public ModuleManager::Instance {
    std::string name;
    SourceManager::SourceHandler handler{};
    dsp::stream<dsp::complex_t> stream;
    std::vector<DeviceEntry> devices;
    std::string devicesCombo;
    int selected = 0;
    char port[32] = "COM10";
    std::atomic<bool> running{false};
    std::thread captureThread;
    std::mutex catMutex;
    std::condition_variable catCv;
    std::thread catThread;
    Cat cat;
    bool catWorkerStop = false;
    bool catPending = false;
    uint64_t pendingCatHz = 0;
    uint64_t lastCatHz = 0;
    std::chrono::steady_clock::time_point lastCatSent{};
    double frequency = 7100000.0;
    bool invertIQ = false;
    bool catEnabled = true; // CAT enabled by default; user may disable it before Play.
    bool enabled = true;

    void catWorker() {
        std::unique_lock<std::mutex> lock(catMutex);
        constexpr auto minCatInterval = std::chrono::milliseconds(200);
        while (!catWorkerStop) {
            catCv.wait(lock, [this] { return catWorkerStop || catPending; });
            if (catWorkerStop) break;
            if (lastCatSent != std::chrono::steady_clock::time_point{}) {
                auto nextSend = lastCatSent + minCatInterval;
                // New tune requests overwrite pendingCatHz while waiting.
                catCv.wait_until(lock, nextSend, [this] { return catWorkerStop; });
                if (catWorkerStop) break;
            }
            const uint64_t hz = pendingCatHz;
            catPending = false;
            if (hz == lastCatHz) continue;
            // Do not hold catMutex during the ~800 ms transmission.
            // New tune callbacks can replace the pending frequency immediately.
            lock.unlock();
            const bool sent = cat.tune(hz);
            lock.lock();
            if (!sent) {
                flog::warn("Selenite: CAT tuning failed");
            } else {
                lastCatHz = hz;
            }
            // Extra 200 ms guard after the completed frame.
            lastCatSent = std::chrono::steady_clock::now();
        }
    }
    void refresh() {
        devices = listInputs(); devicesCombo.clear();
        for (size_t i=0;i<devices.size();++i) {
            if (devices[i].name.find("Selenite") != std::string::npos) selected = (int)i;
            devicesCombo += devices[i].name; devicesCombo.push_back('\0');
        }
        if (devices.empty()) { devicesCombo = "No capture devices"; devicesCombo.push_back('\0'); selected = 0; }
        else selected = std::min(selected, (int)devices.size()-1);
    }
    void capture() {
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(init)) return;
        IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr; IAudioCaptureClient* reader = nullptr;
        WAVEFORMATEX* fmt = nullptr;
        do {
            if (selected >= (int)devices.size()) break;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
            if (FAILED(en->GetDevice(devices[selected].id.c_str(), &dev))) break;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) break;
            if (FAILED(client->GetMixFormat(&fmt))) break;
            // Shared-mode capture uses Windows' current device format. Set Windows recording format to stereo 96 kHz.
            if (fmt->nChannels != 2 || fmt->nSamplesPerSec != 96000) {
                flog::error("Selenite: Windows capture format must be stereo 96000 Hz (actual {} channels, {} Hz)", std::to_string(fmt->nChannels), std::to_string(fmt->nSamplesPerSec));
                break;
            }
            bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            bool isPcm = fmt->wFormatTag == WAVE_FORMAT_PCM;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
                auto* ex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
                isFloat = ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                isPcm = ex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
            }
            if ((!isFloat || fmt->wBitsPerSample != 32) && (!isPcm || fmt->wBitsPerSample != 16)) {
                flog::error("Selenite: unsupported capture sample format ({} bits)", fmt->wBitsPerSample); break;
            }
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, fmt, nullptr))) break;
            if (FAILED(client->GetService(IID_PPV_ARGS(&reader)))) break;
            if (FAILED(client->Start())) break;
            while (running) {
                UINT32 packets=0;
                if (FAILED(reader->GetNextPacketSize(&packets))) break;
                if (!packets) { Sleep(5); continue; }
                BYTE* data=nullptr; UINT32 frames=0; DWORD flags=0;
                if (FAILED(reader->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                UINT32 pos=0;
                while (pos<frames && running) {
                    UINT32 count=std::min<UINT32>(frames-pos, 4096);
                    for (UINT32 i=0;i<count;i++) {
                        float a=0,b=0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                            if (isFloat) { const float* p = reinterpret_cast<const float*>(data); a=p[2*(pos+i)]; b=p[2*(pos+i)+1]; }
                            else { const int16_t* p = reinterpret_cast<const int16_t*>(data); a=p[2*(pos+i)]/32768.f; b=p[2*(pos+i)+1]/32768.f; }
                        }
                        stream.writeBuf[i].re=a; stream.writeBuf[i].im=invertIQ ? -b : b;
                    }
                    if (!stream.swap(count)) { running=false; break; }
                    pos+=count;
                }
                reader->ReleaseBuffer(frames);
            }
            client->Stop();
        } while(false);
        if (fmt) CoTaskMemFree(fmt);
        if (reader) reader->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        CoUninitialize();
    }
    static void select(void* ctx) { core::setInputSampleRate(96000); }
    static void deselect(void* ctx) {}
    static void menu(void* ctx) {
        auto* s=(SeleniteSource*)ctx;
        if (s->running) SmGui::BeginDisabled();
        SmGui::FillWidth();
        SmGui::Combo(("Input##_selenite_"+s->name).c_str(), &s->selected, s->devicesCombo.c_str());
        SmGui::FillWidth();
        if (SmGui::Button(("Refresh##_selenite_"+s->name).c_str())) s->refresh();
        SmGui::InputText(("CAT port##_selenite_"+s->name).c_str(), s->port, sizeof(s->port));
        SmGui::Checkbox(("Enable CAT##_selenite_" + s->name).c_str(), &s->catEnabled);
        SmGui::Checkbox(("Invert Q##_selenite_"+s->name).c_str(), &s->invertIQ);
        if (s->running) SmGui::EndDisabled();
    }
    static void start(void* ctx) {
        auto* s=(SeleniteSource*)ctx;
        if (s->running || s->devices.empty()) return;
        if (s->catEnabled) {
            std::lock_guard<std::mutex> lock(s->catMutex);
            s->lastCatHz = 0;
            s->lastCatSent = {};
            s->catPending = false;
            s->catWorkerStop = false;
            if (!s->cat.open(s->port)) flog::error("Selenite: CAT unavailable; IQ capture will still run");
            else s->catThread = std::thread([s]() { s->catWorker(); });
        }
        s->running=true;
        s->captureThread=std::thread([s](){s->capture();});
    }
    static void stop(void* ctx) {
        auto* s=(SeleniteSource*)ctx;
        s->running=false;
        s->stream.stopWriter();
        if (s->captureThread.joinable()) s->captureThread.join();
        s->stream.clearWriteStop();
        {
            std::lock_guard<std::mutex> lock(s->catMutex);
            s->catWorkerStop = true;
            s->catPending = false;
        }
        s->catCv.notify_one();
        if (s->catThread.joinable()) s->catThread.join();
        std::lock_guard<std::mutex> lock(s->catMutex);
        s->cat.close();
    }
    static void tune(double freq, void* ctx) {
        auto* s = static_cast<SeleniteSource*>(ctx);
        // Log every callback, including unexpected values, before any CAT write.
        flog::info("Selenite: tune callback {} Hz (running={}, CAT={})",
                   std::to_string(freq), s->running.load(), s->catEnabled);
        if (!std::isfinite(freq) || freq < 1500000.0 || freq > 32000000.0) {
            flog::warn("Selenite: rejected out-of-range CAT frequency {} Hz", std::to_string(freq));
            return;
        }
        const uint64_t hz = static_cast<uint64_t>(std::llround(freq));
        if (hz < 1500000ULL || hz > 32000000ULL) {
            flog::warn("Selenite: rejected rounded CAT frequency {} Hz", std::to_string(hz));
            return;
        }
        s->frequency = freq;
        if (!s->running || !s->catEnabled) return;
        {
            std::lock_guard<std::mutex> lock(s->catMutex);
            if (!s->catThread.joinable() || s->catWorkerStop) return;
            if (!s->catPending && hz == s->lastCatHz) return;
            s->pendingCatHz = hz;
            s->catPending = true;
        }
        s->catCv.notify_one();
    }
public:
    explicit SeleniteSource(std::string n):name(std::move(n)) {
        refresh();
        handler.ctx=this; handler.selectHandler=select; handler.deselectHandler=deselect;
        handler.menuHandler=menu; handler.startHandler=start; handler.stopHandler=stop;
        handler.tuneHandler=tune; handler.stream=&stream;
        sigpath::sourceManager.registerSource("Selenite Lite", &handler);
    }
    ~SeleniteSource() override { stop(this); sigpath::sourceManager.unregisterSource("Selenite Lite"); }
    void postInit() override {}
    void enable() override { enabled=true; }
    void disable() override { enabled=false; }
    bool isEnabled() override { return enabled; }
};

MOD_EXPORT void _INIT_() {}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new SeleniteSource(name); }
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) { delete static_cast<SeleniteSource*>(instance); }
MOD_EXPORT void _END_() {}