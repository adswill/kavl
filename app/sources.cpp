// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app sources (sources.h).
#include "sources.h"

#include <libhackrf/hackrf.h>
#include <libusb.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <regex>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

// keep the DC spike out of the signal when the sample rate leaves room: tune half the width plus 100 kHz above the wanted frequency
double dcOffset(double fs, double width) {
    const double off = width / 2.0 + 100e3;
    return off + width / 2.0 + 50e3 < fs / 2.0 ? off : 0.0;
}

// ---- IQ file -------------------------------------------------------------------------------------------------------------------------------------------
class FileSource : public IqSource {
public:
    explicit FileSource(std::string path) : path_(std::move(path)) {}
    ~FileSource() override {
        stop();
        if (f_) std::fclose(f_);
    }
    std::string name() const override { return "IQ file"; }
    double sampleRate() const override { return rate_; }
    bool open() {
        f_ = std::fopen(path_.c_str(), "rb");
        if (!f_) {
            err_ = "Cannot open " + path_;
            return false;
        }
        uint8_t h[12];
        if (std::fread(h, 1, 12, f_) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) {
            err_ = "Not a WAV file";
            return false;
        }
        for (;;) {
            uint8_t c[8];
            if (std::fread(c, 1, 8, f_) != 8) break;
            const uint32_t len = c[4] | (c[5] << 8) | (c[6] << 16) | (static_cast<uint32_t>(c[7]) << 24);
            if (!std::memcmp(c, "fmt ", 4)) {
                std::vector<uint8_t> b(len);
                if (std::fread(b.data(), 1, len, f_) != len) break;
                fmt_ = static_cast<uint16_t>(b[0] | (b[1] << 8));
                ch_ = static_cast<uint16_t>(b[2] | (b[3] << 8));
                rate_ = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
                bits_ = static_cast<uint16_t>(b[14] | (b[15] << 8));
                if (fmt_ == 0xFFFE && len >= 26) fmt_ = static_cast<uint16_t>(b[24] | (b[25] << 8));
            } else if (!std::memcmp(c, "data", 4)) {
                dataStart_ = std::ftell(f_);
                dataLen_ = len;
                break;
            } else {
                std::fseek(f_, len + (len & 1), SEEK_CUR);
            }
        }
        if (dataStart_ < 0 || ch_ != 2 || !((fmt_ == 1 && bits_ == 16) || (fmt_ == 3 && bits_ == 32)) || rate_ <= 0) {
            err_ = "Need a 2-channel 16-bit or float IQ WAV";
            rate_ = 0;
            return false;
        }
        return true;
    }
    bool start(Sink sink) override {
        stop();
        if (rate_ <= 0) return false;
        quit_ = false;
        th_ = std::thread([this, sink] { run(sink); });
        return true;
    }
    void stop() override {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }

private:
    void run(const Sink& sink) {
        const size_t bps = fmt_ == 1 ? 4 : 8;
        const size_t chunk = static_cast<size_t>(rate_ / 100.0);   // 10 ms
        std::vector<uint8_t> raw(chunk * bps);
        std::vector<kavl::cf> iq(chunk);
        std::fseek(f_, dataStart_, SEEK_SET);
        long left = dataLen_;
        auto next = std::chrono::steady_clock::now();
        while (!quit_) {
            if (left <= 0) {
                std::fseek(f_, dataStart_, SEEK_SET);
                left = dataLen_;
            }
            const size_t got = std::fread(raw.data(), 1, std::min(raw.size(), static_cast<size_t>(left)), f_);
            if (got == 0) {
                left = 0;
                continue;
            }
            left -= static_cast<long>(got);
            const size_t n = got / bps;
            for (size_t i = 0; i < n; i++) {
                if (fmt_ == 1) {
                    int16_t a, b;
                    std::memcpy(&a, &raw[4 * i], 2);
                    std::memcpy(&b, &raw[4 * i + 2], 2);
                    iq[i] = kavl::cf(a / 32768.f, b / 32768.f);
                } else {
                    float a, b;
                    std::memcpy(&a, &raw[8 * i], 4);
                    std::memcpy(&b, &raw[8 * i + 4], 4);
                    iq[i] = kavl::cf(a, b);
                }
            }
            sink(iq.data(), n);
            next += std::chrono::microseconds(static_cast<long long>(1e6 * static_cast<double>(n) / rate_));
            std::this_thread::sleep_until(next);
        }
    }
    std::string path_;
    FILE* f_ = nullptr;
    uint16_t fmt_ = 0, ch_ = 0, bits_ = 0;
    double rate_ = 0;
    long dataStart_ = -1, dataLen_ = 0;
    std::thread th_;
    std::atomic<bool> quit_{false};
};

// ---- RTL-SDR through rtl_tcp -----------------------------------------------------------------------------------------------------------------------------
class RtlTcpSource : public IqSource {
public:
    RtlTcpSource(RtlSettings s, double f, double w) : s_(std::move(s)), freq_(f), off_(dcOffset(kRate, w)) {}
    ~RtlTcpSource() override { stop(); }
    std::string name() const override { return s_.remote ? "rtl_tcp " + s_.host : "RTL-SDR"; }
    double sampleRate() const override { return kRate; }
    double offsetHz() const override { return off_; }
    bool start(Sink sink) override {
        stop();
        int port = s_.port;
        if (!s_.remote) {
            const std::string exe = findRtlTcp();
            if (exe.empty()) {
                err_ = "rtl_tcp was not found. Install it with: brew install librtlsdr";
                return false;
            }
            port = 19000 + (getpid() % 1000);
            const std::string sp = std::to_string(port), sd = std::to_string(s_.index), sr = std::to_string(static_cast<long>(kRate)),
                              sf = std::to_string(static_cast<long long>(freq_ + off_));
            std::vector<std::string> args = {exe, "-a", "127.0.0.1", "-p", sp, "-d", sd, "-s", sr, "-f", sf};
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
            const int rc = posix_spawn(&child_, exe.c_str(), &fa, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&fa);
            if (rc != 0) {
                err_ = "rtl_tcp did not start";
                child_ = 0;
                return false;
            }
        }
        // connect (rtl_tcp needs a moment to open the dongle)
        const std::string host = s_.remote ? s_.host : "127.0.0.1";
        for (int attempt = 0; attempt < 50 && fd_ < 0; attempt++) {
            fd_ = connectTo(host, port);
            if (fd_ < 0) {
                if (child_ > 0 && waitpid(child_, nullptr, WNOHANG) == child_) {
                    child_ = 0;
                    err_ = "rtl_tcp stopped: is the RTL-SDR plugged in and free?";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (fd_ < 0) {
            err_ = "Could not connect to rtl_tcp on " + host + ":" + std::to_string(port);
            stop();
            return false;
        }
        uint8_t hdr[12];
        if (!readAll(hdr, 12) || std::memcmp(hdr, "RTL0", 4)) {
            err_ = "The rtl_tcp server did not answer";
            stop();
            return false;
        }
        command(0x02, static_cast<uint32_t>(kRate));
        command(0x01, static_cast<uint32_t>(freq_ + off_));
        command(0x05, static_cast<uint32_t>(s_.ppm));
        command(0x08, s_.agc ? 1 : 0);
        if (s_.gainTenthsDb < 0) {
            command(0x03, 0);   // automatic
        } else {
            command(0x03, 1);
            command(0x04, static_cast<uint32_t>(s_.gainTenthsDb));
        }
        command(0x0e, s_.biasTee ? 1 : 0);
        sink_ = std::move(sink);
        quit_ = false;
        th_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        quit_ = true;
        if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
        if (th_.joinable()) th_.join();
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        if (child_ > 0) {
            kill(child_, SIGTERM);
            for (int i = 0; i < 30 && waitpid(child_, nullptr, WNOHANG) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (waitpid(child_, nullptr, WNOHANG) == 0) {
                kill(child_, SIGKILL);
                waitpid(child_, nullptr, 0);
            }
            child_ = 0;
        }
    }
    void setFrequency(double hz) override {
        freq_ = hz;
        command(0x01, static_cast<uint32_t>(freq_ + off_));
    }
    void update(const RtlSettings& s) override {   // what changed goes to rtl_tcp at once
        if (s.gainTenthsDb != s_.gainTenthsDb) {
            if (s.gainTenthsDb < 0) {
                command(0x03, 0);
            } else {
                if (s_.gainTenthsDb < 0) command(0x03, 1);
                command(0x04, static_cast<uint32_t>(s.gainTenthsDb));
            }
        }
        if (s.ppm != s_.ppm) command(0x05, static_cast<uint32_t>(s.ppm));
        if (s.agc != s_.agc) command(0x08, s.agc ? 1 : 0);
        if (s.biasTee != s_.biasTee) command(0x0e, s.biasTee ? 1 : 0);
        s_.gainTenthsDb = s.gainTenthsDb;
        s_.ppm = s.ppm;
        s_.agc = s.agc;
        s_.biasTee = s.biasTee;
    }

private:
    static constexpr double kRate = 2.4e6;
    static int connectTo(const std::string& host, int port) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
        int fd = -1;
        for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
                close(fd);
                fd = -1;
            }
        }
        freeaddrinfo(res);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        return fd;
    }
    bool readAll(uint8_t* b, size_t n) {
        size_t got = 0;
        while (got < n) {
            const ssize_t r = recv(fd_, b + got, n - got, 0);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }
    void command(uint8_t cmd, uint32_t v) {
        if (fd_ < 0) return;
        const uint8_t b[5] = {cmd, static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        send(fd_, b, 5, 0);
    }
    void run() {
        std::vector<uint8_t> raw(64 * 1024);
        std::vector<kavl::cf> iq;
        size_t carry = 0;   // an odd byte left from the last read
        while (!quit_) {
            const ssize_t r = recv(fd_, raw.data() + carry, raw.size() - carry, 0);
            if (r <= 0) break;
            const size_t n = carry + static_cast<size_t>(r), pairs = n / 2;
            iq.resize(pairs);
            for (size_t i = 0; i < pairs; i++) iq[i] = kavl::cf((raw[2 * i] - 127.5f) / 127.5f, (raw[2 * i + 1] - 127.5f) / 127.5f);
            carry = n & 1;
            if (carry) raw[0] = raw[n - 1];
            if (pairs) sink_(iq.data(), pairs);
        }
    }
    RtlSettings s_;
    double freq_, off_;
    pid_t child_ = 0;
    int fd_ = -1;
    std::thread th_;
    std::atomic<bool> quit_{false};
    Sink sink_;
};

// ---- HackRF ------------------------------------------------------------------------------------------------------------------------------------------------
class HackRfSource : public IqSource {
public:
    HackRfSource(HackRfSettings s, double f, double w) : s_(std::move(s)), freq_(f), off_(dcOffset(kRate, w)) {}
    ~HackRfSource() override { stop(); }
    std::string name() const override { return "HackRF"; }
    double sampleRate() const override { return kRate; }
    double offsetHz() const override { return off_; }
    bool start(Sink sink) override {
        stop();
        if (hackrf_init() != HACKRF_SUCCESS) {
            err_ = "libhackrf failed to start";
            return false;
        }
        inited_ = true;
        const int rc = s_.serial.empty() ? hackrf_open(&dev_) : hackrf_open_by_serial(s_.serial.c_str(), &dev_);
        if (rc != HACKRF_SUCCESS) {
            err_ = "No HackRF found (or it is in use)";
            dev_ = nullptr;
            stop();
            return false;
        }
        hackrf_set_sample_rate(dev_, kRate);
        hackrf_set_baseband_filter_bandwidth(dev_, hackrf_compute_baseband_filter_bw(static_cast<uint32_t>(kRate * 0.75)));
        hackrf_set_freq(dev_, static_cast<uint64_t>(freq_ + off_));
        hackrf_set_amp_enable(dev_, s_.amp ? 1 : 0);
        hackrf_set_lna_gain(dev_, static_cast<uint32_t>(std::clamp(s_.lna / 8 * 8, 0, 40)));
        hackrf_set_vga_gain(dev_, static_cast<uint32_t>(std::clamp(s_.vga / 2 * 2, 0, 62)));
        hackrf_set_antenna_enable(dev_, s_.antennaPower ? 1 : 0);
        sink_ = std::move(sink);
        if (hackrf_start_rx(dev_, [](hackrf_transfer* t) { return static_cast<HackRfSource*>(t->rx_ctx)->onData(t); }, this) != HACKRF_SUCCESS) {
            err_ = "The HackRF did not start receiving";
            stop();
            return false;
        }
        return true;
    }
    void stop() override {
        if (dev_) {
            hackrf_stop_rx(dev_);
            hackrf_set_antenna_enable(dev_, 0);
            hackrf_close(dev_);
            dev_ = nullptr;
        }
        if (inited_) hackrf_exit();
        inited_ = false;
    }
    void setFrequency(double hz) override {
        freq_ = hz;
        if (dev_) hackrf_set_freq(dev_, static_cast<uint64_t>(freq_ + off_));
    }
    void update(const HackRfSettings& s) override {
        if (!dev_) return;
        if (s.lna != s_.lna) hackrf_set_lna_gain(dev_, static_cast<uint32_t>(std::clamp(s.lna / 8 * 8, 0, 40)));
        if (s.vga != s_.vga) hackrf_set_vga_gain(dev_, static_cast<uint32_t>(std::clamp(s.vga / 2 * 2, 0, 62)));
        if (s.amp != s_.amp) hackrf_set_amp_enable(dev_, s.amp ? 1 : 0);
        if (s.antennaPower != s_.antennaPower) hackrf_set_antenna_enable(dev_, s.antennaPower ? 1 : 0);
        s_.lna = s.lna;
        s_.vga = s.vga;
        s_.amp = s.amp;
        s_.antennaPower = s.antennaPower;
    }

private:
    static constexpr double kRate = 4e6;
    int onData(hackrf_transfer* t) {
        const int n = t->valid_length / 2;
        buf_.resize(static_cast<size_t>(n));
        const int8_t* b = reinterpret_cast<const int8_t*>(t->buffer);
        for (int i = 0; i < n; i++) buf_[static_cast<size_t>(i)] = kavl::cf(b[2 * i] / 128.f, b[2 * i + 1] / 128.f);
        sink_(buf_.data(), buf_.size());
        return 0;
    }
    HackRfSettings s_;
    double freq_, off_;
    hackrf_device* dev_ = nullptr;
    bool inited_ = false;
    Sink sink_;
    std::vector<kavl::cf> buf_;
};

}  // namespace

std::vector<RadioDevice> listRtlSdr() {
    // the RTL2832U USB ids librtlsdr knows; rtl_tcp numbers the dongles in this USB order
    static const uint16_t ids[][2] = {{0x0bda, 0x2832}, {0x0bda, 0x2838}, {0x0413, 0x6680}, {0x0413, 0x6f0f}, {0x0458, 0x707f}, {0x0ccd, 0x00a9},
                                      {0x0ccd, 0x00b3}, {0x0ccd, 0x00b4}, {0x0ccd, 0x00b5}, {0x0ccd, 0x00b7}, {0x0ccd, 0x00b8}, {0x0ccd, 0x00b9},
                                      {0x0ccd, 0x00c0}, {0x0ccd, 0x00c6}, {0x0ccd, 0x00d3}, {0x0ccd, 0x00d7}, {0x0ccd, 0x00e0}, {0x1554, 0x5020},
                                      {0x15f4, 0x0131}, {0x15f4, 0x0133}, {0x185b, 0x0620}, {0x185b, 0x0650}, {0x185b, 0x0680}, {0x1b80, 0xd393},
                                      {0x1b80, 0xd394}, {0x1b80, 0xd395}, {0x1b80, 0xd397}, {0x1b80, 0xd398}, {0x1b80, 0xd39d}, {0x1b80, 0xd3a4},
                                      {0x1b80, 0xd3a8}, {0x1b80, 0xd3af}, {0x1b80, 0xd3b0}, {0x1d19, 0x1101}, {0x1d19, 0x1102}, {0x1d19, 0x1103},
                                      {0x1d19, 0x1104}, {0x1f4d, 0xa803}, {0x1f4d, 0xb803}, {0x1f4d, 0xc803}, {0x1f4d, 0xd286}, {0x1f4d, 0xd803}};
    std::vector<RadioDevice> out;
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return out;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    int index = 0;
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        bool known = false;
        for (const auto& id : ids)
            if (d.idVendor == id[0] && d.idProduct == id[1]) known = true;
        if (!known) continue;
        RadioDevice r;
        r.index = index++;
        char prod[128] = {}, serial[128] = {};
        libusb_device_handle* h = nullptr;
        if (libusb_open(list[i], &h) == 0) {
            if (d.iProduct) libusb_get_string_descriptor_ascii(h, d.iProduct, reinterpret_cast<unsigned char*>(prod), sizeof(prod) - 1);
            if (d.iSerialNumber) libusb_get_string_descriptor_ascii(h, d.iSerialNumber, reinterpret_cast<unsigned char*>(serial), sizeof(serial) - 1);
            libusb_close(h);
        }
        r.serial = serial;
        r.label = std::to_string(r.index) + ": " + (prod[0] ? prod : "RTL-SDR") + (serial[0] ? std::string(" (SN ") + serial + ")" : std::string());
        out.push_back(r);
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

std::vector<RadioDevice> listHackRf() {
    std::vector<RadioDevice> out;
    if (hackrf_init() != HACKRF_SUCCESS) return out;
    if (hackrf_device_list_t* l = hackrf_device_list()) {
        for (int i = 0; i < l->devicecount; i++) {
            RadioDevice r;
            r.index = i;
            r.serial = l->serial_numbers[i] ? l->serial_numbers[i] : "";
            const char* kind = l->usb_board_ids[i] == USB_BOARD_ID_HACKRF_ONE ? "HackRF One" : "HackRF";
            r.label = std::string(kind) + (r.serial.size() > 8 ? " ..." + r.serial.substr(r.serial.size() - 8) : std::string(" ") + r.serial);
            out.push_back(r);
        }
        hackrf_device_list_free(l);
    }
    hackrf_exit();
    return out;
}

std::string findRtlTcp() {
    for (const char* p : {"/opt/homebrew/bin/rtl_tcp", "/usr/local/bin/rtl_tcp", "/usr/bin/rtl_tcp"})
        if (std::filesystem::exists(p)) return p;
    return "";
}

std::unique_ptr<IqSource> makeFileSource(const std::string& path) {
    auto s = std::make_unique<FileSource>(path);
    s->open();   // the rate is known from here on; start() reports a failure
    return s;
}

double frequencyFromName(const std::string& path) {
    std::smatch m;
    static const std::regex re("([0-9]{4,12})Hz");
    return std::regex_search(path, m, re) ? std::stod(m[1].str()) : 0.0;
}

std::unique_ptr<IqSource> makeRtlSource(const RtlSettings& s, double f, double w) { return std::make_unique<RtlTcpSource>(s, f, w); }
std::unique_ptr<IqSource> makeHackRfSource(const HackRfSettings& s, double f, double w) { return std::make_unique<HackRfSource>(s, f, w); }
