#include <alsa/asoundlib.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
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
#include <cstdint>
#include <chrono>

SDRPP_MOD_INFO{
    /* Name */ "selenite_source",
    /* Description */ "Selenite Lite ALSA IQ and FT-817 CAT",
    /* Author */ "Community integration",
    /* Version */ 0, 2, 0,
    /* Max instances */ 1
};

namespace {
struct DeviceEntry { std::string id, name; };
std::vector<DeviceEntry> listInputs() {
    std::vector<DeviceEntry> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) >= 0 && hints) {
        for (void** h = hints; *h; ++h) {
            char* n = snd_device_name_get_hint(*h, "NAME");
            char* d = snd_device_name_get_hint(*h, "DESC");
            char* io = snd_device_name_get_hint(*h, "IOID");
            if (n && (!io || std::strcmp(io, "Output") != 0)) {
                std::string id(n);
                // Prefer plughw: for automatic conversion where needed, while
                // requesting the exact stereo 96 kHz format below.
                if (id.rfind("hw:", 0) == 0) id = "plughw:" + id.substr(3);
                std::string label = d ? d : id;
                std::replace(label.begin(), label.end(), '\n', ' ');
                if (std::none_of(out.begin(), out.end(), [&](const DeviceEntry& e){ return e.id == id; }))
                    out.push_back({id, label + " [" + id + "]"});
            }
            if (n) free(n);
            if (d) free(d);
            if (io) free(io);
        }
        snd_device_name_free_hint(hints);
    }
    if (out.empty()) out.push_back({"default", "ALSA default"});
    return out;
}

class Cat {
    int fd = -1;
public:
    ~Cat() { close(); }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
    bool open(const std::string& port) {
        close();
        fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { flog::error("Selenite: CAT open failed: {}", std::string(std::strerror(errno))); return false; }
        struct termios tio{};
        if (tcgetattr(fd, &tio) != 0) { close(); return false; }
        cfmakeraw(&tio);
        cfsetispeed(&tio, B9600); cfsetospeed(&tio, B9600);
        tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
        tio.c_cflag |= CS8 | CLOCAL | CREAD;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
        tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(); return false; }
        int bits = 0;
        if (ioctl(fd, TIOCMGET, &bits) == 0) {
            bits &= ~(TIOCM_DTR | TIOCM_RTS);
            if (ioctl(fd, TIOCMSET, &bits) != 0)
                flog::warn("Selenite: could not clear DTR/RTS");
        } else {
            flog::warn("Selenite: modem control lines unavailable");
        }
        return true;
    }
    bool tune(uint64_t hz) {
        if (fd < 0 || hz < 1500000ULL || hz > 32000000ULL) return false;
        uint64_t units = (hz + 5) / 10;
        uint8_t cmd[5]{};
        for (int i = 3; i >= 0; --i) {
            uint8_t lo = units % 10; units /= 10;
            uint8_t hi = units % 10; units /= 10;
            cmd[i] = (hi << 4) | lo;
        }
        cmd[4] = 0x01;
        char catPacket[64];
        std::snprintf(catPacket, sizeof(catPacket), "%02X %02X %02X %02X %02X",
                      static_cast<unsigned>(cmd[0]), static_cast<unsigned>(cmd[1]),
                      static_cast<unsigned>(cmd[2]), static_cast<unsigned>(cmd[3]),
                      static_cast<unsigned>(cmd[4]));
        flog::info("Selenite: CAT SET {} Hz, packet {}", std::to_string(hz), std::string(catPacket));
        size_t sent = 0;
        while (sent < sizeof(cmd)) {
            ssize_t n = ::write(fd, cmd + sent, sizeof(cmd) - sent);
            if (n > 0) { sent += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            return false;
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
    char port[128] = "/dev/ttyACM0";
    std::atomic<bool> running{false};
    std::thread captureThread;
    std::mutex catMutex;
    Cat cat;
    double frequency = 7100000.0;
    bool invertQ = false;
    bool catEnabled = true; // CAT enabled by default; user may disable it before Play.
    bool catDiagnostic = true; // Safe default: log CAT requests without opening serial or transmitting.
    bool enabled = true;

    void refresh() {
        devices = listInputs();
        devicesCombo.clear();
        selected = 0;
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].name.find("Selenite") != std::string::npos)
                selected = static_cast<int>(i);
            devicesCombo += devices[i].name;
            devicesCombo.push_back('\0');
        }
    }
    void capture() {
        if (selected < 0 || selected >= static_cast<int>(devices.size())) return;
        const std::string device = devices[selected].id;
        snd_pcm_t* pcm = nullptr;
        int rc = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (rc < 0) {
            flog::error("Selenite: ALSA open failed: {}", std::string(snd_strerror(rc)));
            return;
        }
        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        rc = snd_pcm_hw_params_any(pcm, hw);
        if (rc >= 0) rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (rc >= 0) rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        if (rc >= 0) rc = snd_pcm_hw_params_set_channels(pcm, hw, 2);
        if (rc >= 0) rc = snd_pcm_hw_params_set_rate(pcm, hw, 96000, 0);
        if (rc >= 0) rc = snd_pcm_hw_params(pcm, hw);
        if (rc < 0) {
            flog::error("Selenite: ALSA requires 96 kHz stereo S16_LE: {}", std::string(snd_strerror(rc)));
            snd_pcm_close(pcm);
            return;
        }
        rc = snd_pcm_prepare(pcm);
        if (rc < 0) {
            flog::error("Selenite: ALSA prepare failed: {}", std::string(snd_strerror(rc)));
            snd_pcm_close(pcm);
            return;
        }
        constexpr int frames = 1024;
        int16_t buffer[frames * 2];
        while (running) {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, buffer, frames);
            if (n == -EAGAIN || n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            if (n < 0) {
                int recover = snd_pcm_recover(pcm, static_cast<int>(n), 1);
                if (recover < 0) {
                    flog::error("Selenite: ALSA capture error: {}", std::string(snd_strerror(recover)));
                    break;
                }
                continue;
            }
            for (snd_pcm_sframes_t i = 0; i < n; ++i) {
                stream.writeBuf[i].re = buffer[2*i] / 32768.f;
                float q = buffer[2*i + 1] / 32768.f;
                stream.writeBuf[i].im = invertQ ? -q : q;
            }
            if (!stream.swap(static_cast<int>(n))) break;
        }
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    static void select(void*) { core::setInputSampleRate(96000); }
    static void deselect(void*) {}
    static void menu(void* ctx) {
        auto* s = static_cast<SeleniteSource*>(ctx);
        if (s->running) SmGui::BeginDisabled();
        SmGui::FillWidth();
        SmGui::Combo(("Input##_selenite_" + s->name).c_str(), &s->selected, s->devicesCombo.c_str());
        SmGui::FillWidth();
        if (SmGui::Button(("Refresh##_selenite_" + s->name).c_str())) s->refresh();
        SmGui::InputText(("CAT port##_selenite_" + s->name).c_str(), s->port, sizeof(s->port));
        SmGui::Checkbox(("Enable CAT##_selenite_" + s->name).c_str(), &s->catEnabled);
        SmGui::Checkbox(("CAT diagnostic (no TX)##_selenite_" + s->name).c_str(), &s->catDiagnostic);
        SmGui::Checkbox(("Invert Q##_selenite_" + s->name).c_str(), &s->invertQ);
        if (s->running) SmGui::EndDisabled();
    }
    static void start(void* ctx) {
        auto* s = static_cast<SeleniteSource*>(ctx);
        if (s->running || s->devices.empty()) return;
        {
            std::lock_guard<std::mutex> lock(s->catMutex);
            if (s->catEnabled && !s->catDiagnostic && !s->cat.open(s->port))
                flog::warn("Selenite: CAT unavailable; IQ capture will still run");
            // Do not tune on Play; wait for an explicit SDR++ tune callback.
        }
        s->running = true;
        s->captureThread = std::thread([s] { s->capture(); });
    }
    static void stop(void* ctx) {
        auto* s = static_cast<SeleniteSource*>(ctx);
        s->running = false;
        s->stream.stopWriter();
        if (s->captureThread.joinable()) s->captureThread.join();
        s->stream.clearWriteStop();
        std::lock_guard<std::mutex> lock(s->catMutex);
        s->cat.close();
    }
    static void tune(double freq, void* ctx) {
        auto* s = static_cast<SeleniteSource*>(ctx);
        // Log every callback, including unexpected values, before any CAT write.
        flog::info("Selenite: tune callback {} Hz (running={}, CAT={}, diagnostic={})",
                   std::to_string(freq), s->running.load(), s->catEnabled, s->catDiagnostic);
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
        if (s->catDiagnostic) {
            flog::info("Selenite: CAT diagnostic only, would tune {} Hz (no serial write)", std::to_string(hz));
            return;
        }
        std::lock_guard<std::mutex> lock(s->catMutex);
        if (!s->cat.tune(hz)) flog::warn("Selenite: CAT tuning failed");
    }
public:
    explicit SeleniteSource(std::string n) : name(std::move(n)) {
        refresh();
        handler.ctx = this;
        handler.selectHandler = select;
        handler.deselectHandler = deselect;
        handler.menuHandler = menu;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("Selenite Lite", &handler);
    }
    ~SeleniteSource() override {
        stop(this);
        sigpath::sourceManager.unregisterSource("Selenite Lite");
    }
    void postInit() override {}
    void enable() override { enabled = true; }
    void disable() override { enabled = false; }
    bool isEnabled() override { return enabled; }
};

MOD_EXPORT void _INIT_() {}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) { return new SeleniteSource(name); }
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) { delete static_cast<SeleniteSource*>(instance); }
MOD_EXPORT void _END_() {}