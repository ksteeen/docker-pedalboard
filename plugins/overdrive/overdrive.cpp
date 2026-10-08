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

std::atomic<int> g_in_slot(0);
std::atomic<int> g_out_slot(1);

std::atomic<float> g_bypass(1.0f);
std::atomic<float> g_drive(0.45f);
std::atomic<float> g_tone(0.50f);
std::atomic<float> g_level(0.60f);
std::atomic<float> g_mode(1.0f); // 0.0f = LP, 1.0f = HP

struct OverdriveDSP {
    // Вхідний фільтр для зрізу бруду внизу
    float hp_x1 = 0.0f, hp_y1 = 0.0f;
    // Фільтр середини для HP-режиму
    float mid_boost = 0.0f;
    // Тон-стек
    float tone_lp = 0.0f;
    // DC-blocker після перевантаження
    float dc_x1 = 0.0f, dc_y1 = 0.0f;

    inline float mosfet_clip(float x) {
        // Асиметричний софт-кліпінг MOSFET
        if (x > 0.0f) {
            return std::tanh(x * 1.2f) * 0.85f;
        } else {
            // М'якший вигин для негативної напівхвилі (парні гармоніки)
            return (x / (1.0f + std::abs(x) * 0.7f)) * 0.95f;
        }
    }

    inline float process(float in, float drive, float tone, float level, float mode) {
        // 1. High-Pass на вході (70 Гц), щоб низ не гудів
        float clean = in - hp_x1 + 0.991f * hp_y1;
        hp_x1 = in;
        hp_y1 = clean;

        // 2. High Peak режим (підйом верхньої середини + додатковий гейн)
        float drive_mult = 1.0f + drive * 24.0f;
        if (mode > 0.5f) {
            mid_boost += 0.25f * (clean - mid_boost);
            clean = clean + (clean - mid_boost) * 1.4f;
            drive_mult *= 1.35f;
        }

        float saturated = clean * drive_mult;

        // 3. М'яке насичення (MOSFET)
        float clipped = mosfet_clip(saturated);

        // 4. DC-Blocker (прибираємо зміщення нуля після асиметрії)
        float dc_clean = clipped - dc_x1 + 0.995f * dc_y1;
        dc_x1 = clipped;
        dc_y1 = dc_clean;

        // 5. Tone Control (м'який RC Low-Pass)
        float tone_cutoff = std::clamp(0.08f + tone * 0.65f, 0.05f, 0.85f);
        tone_lp += tone_cutoff * (dc_clean - tone_lp);

        // 6. Вихідний рівень
        return tone_lp * (level * 1.5f);
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
                else if (key == "drive") g_drive.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "tone") g_tone.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "level") g_level.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "mode") g_mode.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9003;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    OverdriveDSP dsp;

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
        float cur_drive = g_drive.load(std::memory_order_relaxed);
        float cur_tone = g_tone.load(std::memory_order_relaxed);
        float cur_level = g_level.load(std::memory_order_relaxed);
        float cur_mode = g_mode.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float out = dsp.process(in_sample, cur_drive, cur_tone, cur_level, cur_mode);
                    bus->slots[out_slot][i] = std::clamp(out, -0.98f, 0.98f);
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
