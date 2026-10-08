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

std::atomic<int> g_in_slot(2);
std::atomic<int> g_out_slot(3);

std::atomic<float> g_bypass(1.0f);
std::atomic<float> g_mix(0.40f);
std::atomic<float> g_decay(0.76f);
std::atomic<float> g_shimmer(0.45f);

struct OctavePitchShifter {
    static constexpr int BUF_SIZE = 4096;
    float ring_buffer[BUF_SIZE] = {0};
    int write_ptr = 0;

    float read_ptr1 = 0.0f;
    float read_ptr2 = BUF_SIZE / 2.0f;

    inline float process(float in) {
        ring_buffer[write_ptr] = in;
        write_ptr = (write_ptr + 1) % BUF_SIZE;

        read_ptr1 += 2.0f;
        if (read_ptr1 >= BUF_SIZE) read_ptr1 -= BUF_SIZE;

        read_ptr2 += 2.0f;
        if (read_ptr2 >= BUF_SIZE) read_ptr2 -= BUF_SIZE;

        auto get_sample = [&](float ptr) {
            int i0 = static_cast<int>(ptr);
            int i1 = (i0 + 1) % BUF_SIZE;
            float frac = ptr - i0;
            return ring_buffer[i0] + frac * (ring_buffer[i1] - ring_buffer[i0]);
        };

        float s1 = get_sample(read_ptr1);
        float s2 = get_sample(read_ptr2);

        float w1 = 0.5f * (1.0f - std::cos(2.0f * M_PI * (read_ptr1 / BUF_SIZE)));
        float w2 = 0.5f * (1.0f - std::cos(2.0f * M_PI * (read_ptr2 / BUF_SIZE)));

        return (s1 * w1 + s2 * w2);
    }
};

struct AllPass {
    std::vector<float> buf;
    int idx = 0;
    float g = 0.5f;

    void init(int size, float gain = 0.5f) {
        buf.assign(size, 0.0f);
        idx = 0;
        g = gain;
    }

    inline float process(float in) {
        float out_buf = buf[idx];
        float v = in + out_buf * g;
        float out = -v * g + out_buf;
        buf[idx] = v;
        idx = (idx + 1) % buf.size();
        return out;
    }
};

struct ShimmerDSP {
    AllPass diffusers[4];
    OctavePitchShifter pitch_shifter;

    std::vector<float> tank1;
    std::vector<float> tank2;
    int t1_idx = 0;
    int t2_idx = 0;

    float lp_state = 0.0f;
    float hp_x1 = 0.0f;
    float hp_y1 = 0.0f;
    float last_reverb_out = 0.0f;

    ShimmerDSP() {
        diffusers[0].init(317, 0.7f);
        diffusers[1].init(443, 0.7f);
        diffusers[2].init(829, 0.6f);
        diffusers[3].init(1079, 0.6f);

        tank1.assign(3323, 0.0f);
        tank2.assign(4153, 0.0f);
    }

    inline float process(float in, float decay, float shimmer_amount) {
        float pitched = pitch_shifter.process(last_reverb_out);

        float hp_in = pitched - hp_x1 + 0.96f * hp_y1;
        hp_x1 = pitched;
        hp_y1 = hp_in;

        float input = in + hp_in * (shimmer_amount * 1.5f);

        float diffused = input;
        for (int i = 0; i < 4; ++i) diffused = diffusers[i].process(diffused);

        float t1_out = tank1[t1_idx];
        float t2_out = tank2[t2_idx];

        lp_state += 0.55f * (t2_out - lp_state);

        tank1[t1_idx] = diffused + std::tanh(lp_state * decay);
        tank2[t2_idx] = t1_out * decay;

        t1_idx = (t1_idx + 1) % tank1.size();
        t2_idx = (t2_idx + 1) % tank2.size();

        float reverb_tail = (t1_out + t2_out) * 0.5f;
        last_reverb_out = reverb_tail;

        return (reverb_tail + hp_in * shimmer_amount * 0.8f) * 1.8f;
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
                else if (key == "mix") g_mix.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "decay") g_decay.store(std::stof(val), std::memory_order_relaxed);
                else if (key == "shimmer") g_shimmer.store(std::stof(val), std::memory_order_relaxed);
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9002;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    ShimmerDSP shimmer;

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
        float mix = g_mix.load(std::memory_order_relaxed);
        float decay = g_decay.load(std::memory_order_relaxed);
        float shim_amt = g_shimmer.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float wet = shimmer.process(in_sample, decay, shim_amt);
                    float out = in_sample * 0.9f + wet * mix;
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
