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
std::atomic<float> g_mix(0.35f);
std::atomic<float> g_decay(0.75f);
std::atomic<float> g_tone(0.28f);

struct AllPass {
    std::vector<float> buf;
    int idx = 0;
    float g = 0.6f;
    void init(int size, float gain = 0.6f) { buf.assign(size, 0.0f); idx = 0; g = gain; }
    inline float process(float in) {
        float buf_out = buf[idx];
        float v = in + buf_out * g;
        float out = -v * g + buf_out;
        buf[idx] = v;
        idx = (idx + 1) % buf.size();
        return out;
    }
};

struct SpringLine {
    std::vector<float> buf;
    int idx = 0;
    float lp_state = 0.0f;
    void init(int size) { buf.assign(size, 0.0f); idx = 0; lp_state = 0.0f; }
    inline float process(float in, float decay, float damp) {
        float delayed = buf[idx];
        lp_state += damp * (delayed - lp_state);
        buf[idx] = in + lp_state * decay;
        idx = (idx + 1) % buf.size();
        return delayed;
    }
};

struct SpringReverbDSP {
    AllPass ap[3];
    SpringLine spring1, spring2;

    SpringReverbDSP() {
        ap[0].init(153, 0.65f);
        ap[1].init(231, 0.60f);
        ap[2].init(372, 0.55f);
        spring1.init(1511);
        spring2.init(1889);
    }

    inline float process(float in, float decay, float tone) {
        float dispersed = in;
        for (int i = 0; i < 3; ++i) dispersed = ap[i].process(dispersed);

        float damp = std::clamp(tone, 0.05f, 0.6f);
        float s1 = spring1.process(dispersed, decay, damp);
        float s2 = spring2.process(dispersed, decay, damp);
        return (s1 - s2) * 0.7f;
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
    int port = (argc > 3) ? std::stoi(argv[3]) : 9001;

    std::thread(udp_control_server, port).detach();

    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open("/pedal_bus", O_RDWR, 0666);
        if (shm_fd < 0) usleep(5000);
    }

    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    SpringReverbDSP reverb;

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
        float tone = g_tone.load(std::memory_order_relaxed);

        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            if (res == 0) {
                float in_sample = bus->slots[in_slot][i];
                if (active) {
                    float wet = reverb.process(in_sample, decay, tone);
                    float out = in_sample * (1.0f - mix * 0.5f) + wet * mix;
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
