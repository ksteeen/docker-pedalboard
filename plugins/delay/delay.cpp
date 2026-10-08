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

std::atomic<int> g_in_slot(3);
std::atomic<int> g_out_slot(4);

// За замовчуванням стартуємо вимкненим (bypass = 0)
std::atomic<float> g_bypass(0.0f);
std::atomic<float> g_time(350.0f);    // 40 - 1000 мс
std::atomic<float> g_feedback(0.45f); // 0.0 - 0.95
std::atomic<float> g_mix(0.35f);      // 0.0 - 1.0
std::atomic<float> g_tone(0.40f);     // 0.1 - 0.9 (Damp / Темніючий хвіст)

struct AnalogDelayDSP {
    static constexpr int BUF_SIZE = 65536; // ~1.36 секунди на 48 кГц
    float buffer[BUF_SIZE] = {0.0f};
    int write_ptr = 0;

    float smoothed_delay_samples = 16800.0f; // 350 мс
    float filter_state = 0.0f;
    float dc_x1 = 0.0f, dc_y1 = 0.0f;

    inline float read_linear(float r_ptr) {
        int i0 = static_cast<int>(r_ptr);
        int i1 = (i0 + 1) & (BUF_SIZE - 1);
        float frac = r_ptr - i0;
        return buffer[i0] + frac * (buffer[i1] - buffer[i0]);
    }

    inline float process(float in, float time_ms, float fb, float mix, float tone) {
        // 1. Плавне згладжування зміни часу затримки (імітація швидкості мотора плівки)
        float target_samples = std::clamp(time_ms * 48.0f, 1920.0f, 48000.0f);
        smoothed_delay_samples += 0.001f * (target_samples - smoothed_delay_samples);

        float read_ptr = static_cast<float>(write_ptr) - smoothed_delay_samples;
        if (read_ptr < 0.0f) read_ptr += BUF_SIZE;

        float delayed_sample = read_linear(read_ptr);

        // 2. Аналогова фільтрація хвоста (темніє з кожним повтором)
        float damp = std::clamp(1.0f - tone, 0.15f, 0.85f);
        filter_state += (1.0f - damp) * (delayed_sample - filter_state);

        // 3. Плівкове насичення у зворотному зв'язку (із захистом від нескінченного піку)
        float fb_signal = std::tanh(filter_state * fb * 1.05f);

        // DC Blocker
        float dc_clean = fb_signal - dc_x1 + 0.995f * dc_y1;
        dc_x1 = fb_signal;
        dc_y1 = dc_clean;

        // Запис у кільцевий буфер: вхідний сигнал + зациклений повтор
        buffer[write_ptr] = in + dc_clean;
        write_ptr = (write_ptr + 1) & (BUF_SIZE - 1);

        // Вихідний мікс
        return in * (1.0f - mix * 0.4f) + delayed_sample * mix;
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
                else if (key == "time") g_time.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "feedback") g_feedback.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "mix") g_mix.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "tone") g_tone.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9005;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    AnalogDelayDSP dsp;

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
        float cur_time = g_time.load(std::memory_order_relaxed);
        float cur_fb = g_feedback.load(std::memory_order_relaxed);
        float cur_mix = g_mix.load(std::memory_order_relaxed);
        float cur_tone = g_tone.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float out = dsp.process(in_sample, cur_time, cur_fb, cur_mix, cur_tone);
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
