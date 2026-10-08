#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <alsa/asoundlib.h>
#include "../include/shm_bus.h"

constexpr unsigned int SAMPLE_RATE = 48000;
constexpr snd_pcm_uframes_t PERIOD_SIZE = 256;
constexpr unsigned int PERIODS = 4;
constexpr float INPUT_PREAMP_GAIN = 3.2f;

std::atomic<int> g_out_slot(8);

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
            if (eq != std::string::npos && msg.substr(0, eq) == "out_slot") {
                int slot = std::stoi(msg.substr(eq + 1));
                g_out_slot.store(slot, std::memory_order_relaxed);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    const char* env_dev = std::getenv("AUDIO_DEVICE");
    std::string device = (argc > 1 && argv[1][0] != '-') ? argv[1] : (env_dev ? env_dev : "hw:Go,0");
    if (argc > 2) g_out_slot.store(std::stoi(argv[2]));

    std::thread(udp_control_server, 9009).detach();

    shm_unlink("/pedal_bus");
    int shm_fd = shm_open("/pedal_bus", O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) return 1;
    ftruncate(shm_fd, sizeof(PedalBus));
    PedalBus* bus = (PedalBus*)mmap(nullptr, sizeof(PedalBus), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    for (size_t s = 0; s < MAX_SLOTS; ++s) {
        sem_init(&bus->sem[s], 1, 0);
        for (size_t i = 0; i < BLOCK_SIZE; ++i) bus->slots[s][i] = 0.0f;
    }

    snd_pcm_t *cap_handle, *play_handle;
    snd_pcm_hw_params_t *hw_params;

    if (snd_pcm_open(&cap_handle, device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0 ||
        snd_pcm_open(&play_handle, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        return 1;
    }

    auto config_pcm = [&](snd_pcm_t *handle) {
        snd_pcm_hw_params_alloca(&hw_params);
        snd_pcm_hw_params_any(handle, hw_params);
        snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE);
        snd_pcm_hw_params_set_rate(handle, hw_params, SAMPLE_RATE, 0);
        snd_pcm_hw_params_set_channels(handle, hw_params, 2);
        snd_pcm_hw_params_set_period_size(handle, hw_params, PERIOD_SIZE, 0);
        snd_pcm_hw_params_set_periods(handle, hw_params, PERIODS, 0);
        return snd_pcm_hw_params(handle, hw_params) >= 0;
    };

    if (!config_pcm(cap_handle) || !config_pcm(play_handle)) return 1;

    std::vector<int32_t> silence(PERIOD_SIZE * 2, 0);
    for (unsigned int i = 0; i < PERIODS; ++i) snd_pcm_writei(play_handle, silence.data(), PERIOD_SIZE);

    snd_pcm_start(cap_handle);
    snd_pcm_start(play_handle);

    std::vector<int32_t> alsa_buf(PERIOD_SIZE * 2);

    while (true) {
        snd_pcm_sframes_t r = snd_pcm_readi(cap_handle, alsa_buf.data(), PERIOD_SIZE);
        if (r == -EPIPE) { snd_pcm_prepare(cap_handle); continue; }
        else if (r < 0) { r = snd_pcm_recover(cap_handle, r, 0); continue; }

        for (size_t i = 0; i < PERIOD_SIZE; ++i) {
            float raw = (alsa_buf[i * 2] / 2147483648.0f) * INPUT_PREAMP_GAIN;
            bus->slots[0][i] = std::clamp(raw, -0.98f, 0.98f);
        }

        // Дрейнуємо залишки перед сигналом
        sem_post(&bus->sem[0]);

        int out_slot = g_out_slot.load(std::memory_order_relaxed);
        if (out_slot > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 25000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(&bus->sem[out_slot], &ts);
        }

        for (size_t i = 0; i < PERIOD_SIZE; ++i) {
            float out_sample = bus->slots[out_slot][i];
            int32_t out_int = static_cast<int32_t>(out_sample * 2147483647.0f);
            alsa_buf[i * 2] = out_int;
            alsa_buf[i * 2 + 1] = out_int;
        }

        snd_pcm_sframes_t w = snd_pcm_writei(play_handle, alsa_buf.data(), PERIOD_SIZE);
        if (w == -EPIPE) snd_pcm_prepare(play_handle);
        else if (w < 0) snd_pcm_recover(play_handle, w, 0);
    }

    return 0;
}
