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
std::atomic<float> g_fuzz(12.0f);
std::atomic<float> g_tone(0.35f);
std::atomic<float> g_level(0.5f);

struct FuzzDSP {
    float dc_x1 = 0.0f, dc_y1 = 0.0f;
    float sag = 0.0f;
    float lp_state = 0.0f;

    inline float process(float in, float fuzz_amt, float tone, float level) {
        float clean = in - dc_x1 + 0.992f * dc_y1;
        dc_x1 = in;
        dc_y1 = clean;

        float pre_gain = fuzz_amt * 8.0f;
        float x = clean * pre_gain;

        sag += (std::abs(x) * 0.002f - sag) * 0.001f;
        float biased_x = x - (sag * 1.5f);

        float clipped;
        if (biased_x > 0.45f) clipped = 0.45f + 0.1f * std::tanh((biased_x - 0.45f) * 2.5f);
        else if (biased_x < -0.35f) clipped = -0.35f - 0.15f * std::tanh((-biased_x - 0.35f) * 2.0f);
        else clipped = biased_x;

        lp_state += tone * (clipped - lp_state);
        return lp_state * (level * 1.6f);
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
                else if (key == "fuzz") g_fuzz.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "tone") g_tone.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "level") g_level.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9000;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    FuzzDSP dsp;

    while (true) {
        int in_slot = g_in_slot.load(std::memory_order_relaxed);
        int out_slot = g_out_slot.load(std::memory_order_relaxed);

        // Таймаут очікування 6 мс, щоб не зависати при зміні роуту
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 25000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        int res = sem_timedwait(&bus->sem[in_slot], &ts);

        // Якщо встигли отримати кадр — обробляємо DSP, якщо таймаут — пускаємо тишу
        bool active = (g_bypass.load(std::memory_order_relaxed) > 0.5f);
        float cur_fuzz = g_fuzz.load(std::memory_order_relaxed);
        float cur_tone = g_tone.load(std::memory_order_relaxed);
        float cur_level = g_level.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float out = dsp.process(in_sample, cur_fuzz, cur_tone, cur_level);
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
