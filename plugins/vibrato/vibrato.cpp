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

std::atomic<int> g_in_slot(1);
std::atomic<int> g_out_slot(2);

std::atomic<float> g_bypass(1.0f);
std::atomic<float> g_rate(2.5f);     // 0.3 - 8.0 Hz
std::atomic<float> g_depth(0.45f);   // 0.0 - 1.0
std::atomic<float> g_blend(1.0f);    // 0.0 - 1.0 (1.0 = Pure Vibrato, 0.5 = Rich Chorus)
std::atomic<float> g_flutter(0.35f); // 0.0 - 1.0 (плівкове мікро-тремтіння)

struct TapeVibratoDSP {
    static constexpr int BUF_SIZE = 2048;
    float delay_buf[BUF_SIZE] = {0.0f};
    int write_ptr = 0;

    float lfo_phase = 0.0f;
    float flutter_phase = 0.0f;
    float flutter_target = 0.0f;
    float flutter_current = 0.0f;

    float tape_filter = 0.0f;
    uint32_t rng_state = 123456789;

    inline float xorshift() {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        return (rng_state & 0xFFFF) / 65536.0f;
    }

    // Кубічна 4-точкова Hermite інтерполяція для ідеально гладкого пітчу
    inline float read_hermite(float r_ptr) {
        int i0 = static_cast<int>(std::floor(r_ptr));
        float frac = r_ptr - i0;

        int idx_m1 = (i0 - 1 + BUF_SIZE) & (BUF_SIZE - 1);
        int idx_0  = (i0 + BUF_SIZE) & (BUF_SIZE - 1);
        int idx_1  = (i0 + 1 + BUF_SIZE) & (BUF_SIZE - 1);
        int idx_2  = (i0 + 2 + BUF_SIZE) & (BUF_SIZE - 1);

        float y_m1 = delay_buf[idx_m1];
        float y0   = delay_buf[idx_0];
        float y1   = delay_buf[idx_1];
        float y2   = delay_buf[idx_2];

        float c0 = y0;
        float c1 = 0.5f * (y1 - y_m1);
        float c2 = y_m1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - y_m1) + 1.5f * (y0 - y1);

        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    inline float process(float in, float rate, float depth, float blend, float flutter) {
        // М'яка плівкова сатурація + зріз ультразвуку
        float saturated = std::tanh(in * 1.1f) * 0.92f;
        tape_filter += 0.42f * (saturated - tape_filter);

        delay_buf[write_ptr] = tape_filter;

        // Генерація LFO
        lfo_phase += (rate * 2.0f * M_PI) / 48000.0f;
        if (lfo_phase >= 2.0f * M_PI) lfo_phase -= 2.0f * M_PI;

        // Псевдовипадковий дрейф (Tape Flutter)
        flutter_phase += 0.002f;
        if (flutter_phase >= 1.0f) {
            flutter_phase = 0.0f;
            flutter_target = (xorshift() - 0.5f) * 2.0f;
        }
        flutter_current += 0.004f * (flutter_target - flutter_current);

        // Базова затримка 8 мс (384 семпли) + LFO розмах до 4 мс (192 семпли)
        float mod = std::sin(lfo_phase) * (depth * 170.0f);
        float flutter_mod = flutter_current * (flutter * 28.0f);
        float total_delay = 384.0f + mod + flutter_mod;

        float read_ptr = static_cast<float>(write_ptr) - total_delay;
        if (read_ptr < 0.0f) read_ptr += BUF_SIZE;

        float wet = read_hermite(read_ptr);

        write_ptr = (write_ptr + 1) & (BUF_SIZE - 1);

        // Blend: 1.0 = Pure Vibrato; 0.5 = Classic Stereo Chorus feel
        return in * (1.0f - blend * 0.5f) + wet * blend;
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
                else if (key == "rate") g_rate.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "depth") g_depth.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "blend") g_blend.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "flutter") g_flutter.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9004;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    TapeVibratoDSP dsp;

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
        float cur_rate = g_rate.load(std::memory_order_relaxed);
        float cur_depth = g_depth.load(std::memory_order_relaxed);
        float cur_blend = g_blend.load(std::memory_order_relaxed);
        float cur_flutter = g_flutter.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float out = dsp.process(in_sample, cur_rate, cur_depth, cur_blend, cur_flutter);
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
