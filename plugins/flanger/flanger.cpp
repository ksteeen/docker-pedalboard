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

std::atomic<float> g_bypass(0.0f);
std::atomic<float> g_rate(0.35f);    // 0.05 - 4.0 Hz
std::atomic<float> g_depth(0.70f);   // 0.0 - 1.0
std::atomic<float> g_regen(0.65f);   // 0.0 - 0.92 (резонанс)
std::atomic<float> g_manual(0.40f);  // 0.0 - 1.0 (зміщення затримки)

struct FlangerDSP {
    static constexpr int BUF_SIZE = 2048;
    float delay_buf[BUF_SIZE] = {0.0f};
    int write_ptr = 0;

    float lfo_phase = 0.0f;
    float fb_sample = 0.0f;

    // Лінійна інтерполяція для ультракоротких ліній
    inline float read_linear(float ptr) {
        int i0 = static_cast<int>(ptr);
        int i1 = (i0 + 1) & (BUF_SIZE - 1);
        float frac = ptr - i0;
        return delay_buf[i0] + frac * (delay_buf[i1] - delay_buf[i0]);
    }

    inline float process(float in, float rate, float depth, float regen, float manual) {
        // LFO (трикутно-синусоподібне для плавного переходу)
        lfo_phase += (rate * 2.0f * M_PI) / 48000.0f;
        if (lfo_phase >= 2.0f * M_PI) lfo_phase -= 2.0f * M_PI;

        float lfo = 0.5f * (1.0f + std::sin(lfo_phase));

        // Діапазон затримки: від ~0.5 мс (24 семпли) до ~8 мс (384 семпли)
        float base_delay = 24.0f + manual * 180.0f;
        float mod_delay = lfo * (depth * 220.0f);
        float total_delay = base_delay + mod_delay;

        float read_ptr = static_cast<float>(write_ptr) - total_delay;
        if (read_ptr < 0.0f) read_ptr += BUF_SIZE;

        float wet = read_linear(read_ptr);

        // Сатурація петлі зворотного зв'язку, щоб металевий резонанс не кліпував
        fb_sample = std::tanh(wet * regen);
        delay_buf[write_ptr] = in + fb_sample;
        write_ptr = (write_ptr + 1) & (BUF_SIZE - 1);

        // Класичний Flanger mix (сухий + мокрий у фазі)
        return in * 0.7f + wet * 0.7f;
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
                else if (key == "regen") g_regen.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "manual") g_manual.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9007;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    FlangerDSP dsp;

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
        float cur_regen = g_regen.load(std::memory_order_relaxed);
        float cur_manual = g_manual.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float out = dsp.process(in_sample, cur_rate, cur_depth, cur_regen, cur_manual);
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
