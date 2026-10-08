#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <thread>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../../include/shm_bus.h"

std::atomic<int> g_in_slot(6);
std::atomic<int> g_out_slot(7);

std::atomic<float> g_bypass(1.0f);     // За замовчуванням увімкнений (кабсім потрібен завжди)
std::atomic<float> g_type(1.0f);       // 1.0 = 4x12 UK Stack, 0.0 = 2x12 US Combo
std::atomic<float> g_presence(0.50f);  // Яскравість
std::atomic<float> g_volume(0.85f);    // Рівень

struct BiquadFilter {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void set_lowpass(float f0, float q, float fs = 48000.0f) {
        float w0 = 2.0f * M_PI * f0 / fs;
        float alpha = std::sin(w0) / (2.0f * q);
        float cos_w = std::cos(w0);
        float a0 = 1.0f + alpha;
        b0 = ((1.0f - cos_w) / 2.0f) / a0;
        b1 = (1.0f - cos_w) / a0;
        b2 = ((1.0f - cos_w) / 2.0f) / a0;
        a1 = (-2.0f * cos_w) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void set_peaking(float f0, float q, float gain_db, float fs = 48000.0f) {
        float w0 = 2.0f * M_PI * f0 / fs;
        float alpha = std::sin(w0) / (2.0f * q);
        float A = std::pow(10.0f, gain_db / 40.0f);
        float cos_w = std::cos(w0);
        float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0;
        b1 = (-2.0f * cos_w) / a0;
        b2 = (1.0f - alpha * A) / a0;
        a1 = (-2.0f * cos_w) / a0;
        a2 = (1.0f - alpha / A) / a0;
    }

    inline float process(float in) {
        float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }
};

struct CabSimDSP {
    // 4-полюсний Low-Pass (дві біквадратні ланки для спаду -24 dB/oct)
    BiquadFilter lp1, lp2;
    // Резонанс корпусу (Speaker Body Thump ~115 Hz)
    BiquadFilter thump;
    // Верхня середина / Presence (~3.2 kHz)
    BiquadFilter pres_filter;

    float hp_x1 = 0.0f, hp_y1 = 0.0f;
    float current_type = -1.0f;
    float current_pres = -1.0f;

    void update_coefficients(float type, float presence) {
        if (std::abs(type - current_type) < 0.01f && std::abs(presence - current_pres) < 0.01f) return;
        current_type = type;
        current_pres = presence;

        if (type > 0.5f) {
            // 4x12 UK Stack (Celestion V30 style): зріз на 4.4 кГц, жирний низ на 115 Гц
            lp1.set_lowpass(4400.0f, 0.707f);
            lp2.set_lowpass(4800.0f, 0.85f);
            thump.set_peaking(115.0f, 2.2f, 4.5f);
            pres_filter.set_peaking(3200.0f, 1.8f, -2.0f + presence * 8.0f);
        } else {
            // 2x12 US Combo (Fender/Jensen style): відкритіший верх (5.2 кГц), легший низ
            lp1.set_lowpass(5200.0f, 0.707f);
            lp2.set_lowpass(5600.0f, 0.707f);
            thump.set_peaking(95.0f, 1.8f, 2.0f);
            pres_filter.set_peaking(2800.0f, 1.5f, -3.0f + presence * 6.0f);
        }
    }

    inline float process(float in, float type, float presence, float vol) {
        update_coefficients(type, presence);

        // 1. High-Pass зріз непотрібного саб-басу (< 75 Гц)
        float hp = in - hp_x1 + 0.990f * hp_y1;
        hp_x1 = in; hp_y1 = hp;

        // 2. Резонанс кабінету
        float s1 = thump.process(hp);

        // 3. Зона Presence
        float s2 = pres_filter.process(s1);

        // 4. Потужний зріз піску (-24 dB/oct)
        float cab = lp2.process(lp1.process(s2));

        return std::clamp(cab * vol, -0.98f, 0.98f);
    }
};

void udp_control_server(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    char buf[128];

    while (true) {
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = 0;
            std::string msg(buf);
            auto eq = msg.find('=');
            if (eq != std::string::npos) {
                std::string key = msg.substr(0, eq);
                std::string val = msg.substr(eq + 1);

                if (key == "route") {
                    auto colon = val.find(':');
                    if (colon != std::string::npos) {
                        g_in_slot.store(std::stoi(val.substr(0, colon)), std::memory_order_relaxed);
                        g_out_slot.store(std::stoi(val.substr(colon + 1)), std::memory_order_relaxed);
                    }
                }
                else if (key == "bypass") g_bypass.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "type") g_type.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "presence") g_presence.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "volume") g_volume.store(std::stof(val), std::memory_order_relaxed);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    const char* env_active = std::getenv("PEDAL_ACTIVE");
    if (env_active) {
        g_bypass.store(std::string(env_active) == "1" ? 1.0f : 0.0f);
    }

    if (argc > 1) g_in_slot.store(std::stoi(argv[1]));
    if (argc > 2) g_out_slot.store(std::stoi(argv[2]));
    int port = (argc > 3) ? std::stoi(argv[3]) : 9008;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    CabSimDSP dsp;

    while (true) {
        int in_slot = g_in_slot.load(std::memory_order_relaxed);
        int out_slot = g_out_slot.load(std::memory_order_relaxed);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 25000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        int res = sem_timedwait(&bus->sem[in_slot], &ts);

        bool active = (g_bypass.load(std::memory_order_relaxed) > 0.5f);
        float cur_type = g_type.load(std::memory_order_relaxed);
        float cur_pres = g_presence.load(std::memory_order_relaxed);
        float cur_vol = g_volume.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    bus->slots[out_slot][i] = dsp.process(in_sample, cur_type, cur_pres, cur_vol);
                } else {
                    bus->slots[out_slot][i] = in_sample;
                }
            } else {
                bus->slots[out_slot][i] = 0.0f;
            }
        }

        sem_post(&bus->sem[out_slot]);
    }

    return 0;
}
