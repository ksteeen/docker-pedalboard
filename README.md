# Docker Pedalboard 🎸🐳

A modular, real-time digital guitar multi-effects processor running entirely inside Docker containers on Linux. Tested on an Orange Pi Zero 3 (arm64).

Each audio effect runs as an **isolated microservice**. Audio blocks are passed between plugins through **POSIX shared memory (`/dev/shm`) and semaphores**, while parameters and drag-and-drop pedal reordering are controlled over UDP from a web dashboard.

<!-- TODO: add a screenshot of the web dashboard here, e.g.
![Dashboard](docs/dashboard.png)
-->

---

## ⚡ Key Highlights

- **Low-latency shared-memory audio bus:** inter-container audio streaming via mapped shared memory (`/dev/shm/pedal_bus`), synchronized with POSIX semaphores.
- **Dynamic reordering:** rearrange the pedal order from the web interface in real time, without restarting containers.
- **Microservice architecture:** every pedal is a standalone container with its own C++17 DSP implementation. Add or replace pedals without rebuilding the whole system.
- **Multi-arch images:** Docker Hub images are built for both `linux/arm64` and `linux/amd64`.
- **Web UI:** responsive control interface for desktop and mobile browsers.

### Latency

The engine runs at 48 kHz with a 256-sample period, which is ≈ 5.3 ms per audio buffer. Each plugin in the chain hands its block to the next one through shared memory, so total latency depends on the number of active pedals and on the audio interface.

<!-- TODO: add your measured round-trip latency, e.g. "Measured round trip on Orange Pi Zero 3 with all 8 pedals: X ms" -->

---

## 🎛️ Included Effects Chain

| Order | Pedal Name | Category | Controls |
|---|---|---|---|
| **#1** | **Obsessive Drive** | Distortion / Overdrive | Drive, Tone, Level, HP/LP mode |
| **#2** | **Fuzz Face** | Vintage Fuzz | Fuzz, Tone, Level |
| **#3** | **Jet Flanger** | Modulation | Speed, Depth, Resonance, Manual |
| **#4** | **Tape Vibrato** | Modulation / Chorus | Speed, Depth, Flutter, Blend |
| **#5** | **Echo Tape Delay** | Time / Delay | Time (ms), Repeats, Mix, Warmth |
| **#6** | **Spring Reverb** | Reverb | Mix, Decay, Tone |
| **#7** | **Celestial Shimmer** | Pitch / Reverb | Mix, Decay, Shimmer Pitch |
| **#8** | **Cab Sim DI Box** | Cabinet Simulator | Cab Type (US/UK), Presence, Volume |

---

## ✅ Tested Platforms

| Platform | Architecture | Status |
|---|---|---|
| Orange Pi Zero 3 | arm64 | Tested |
| Desktop Linux (x86_64) | amd64 | Image built, not tested |
| Raspberry Pi | arm64 | Image built, not tested |

Multi-arch images are published, but only the Orange Pi Zero 3 has been verified on real hardware. Reports from other systems are welcome.

---

## 🚀 Quick Start (Pre-built Images)

### Prerequisites

- A Linux host (Ubuntu, Debian, Armbian, etc.)
- Docker and Docker Compose installed
- A class-compliant USB audio interface or sound card

### 1. Download `docker-compose.yml`

```bash
mkdir pedal-system && cd pedal-system
curl -O https://raw.githubusercontent.com/ksteeen/pedal-system/main/docker-compose.yml
```

### 2. Identify your audio device

List the available capture cards:

```bash
arecord -l
```

The default device is `hw:Go,0`. If yours is different, set its name in the `AUDIO_DEVICE` environment variable.

### 3. Launch

```bash
docker compose up -d
```

Open `http://<YOUR_DEVICE_IP>` in your browser to access the control panel.

---

## 🛠️ Building from Source

If you want to modify the DSP algorithms or build your own plugins:

```bash
git clone https://github.com/ksteeen/pedal-system.git
cd pedal-system
docker compose -f docker-compose.build.yml up -d --build
```

---

## 📐 Architecture

```text
                 +--------------------------------------+
                 |      Web Dashboard (FastAPI / JS)    |
                 +-------------------+------------------+
                                     | UDP commands
                                     v
+-------------+      +--------------------------------------+      +-------------+
| ALSA Input  | ---> | Slot 0 -> Slot 1 -> ... -> Slot 8    | ---> | ALSA Output |
| (Guitar In) |      |        Shared Memory (/dev/shm)      |      | (Out / Cabs)|
+-------------+      +--------------------------------------+      +-------------+
                                     ^
                     Synchronized via POSIX semaphores
```

- **`engine`**: manages the ALSA capture and playback streams (48 kHz, 256-sample period) and creates the `/pedal_bus` shared memory segment.
- **`plugins/*`**: each plugin reads its input slot from shared memory, processes the block with custom C++17 DSP, and posts the result to the next slot.
- **`dashboard`**: inspects running pedal containers through the Docker socket, renders the controls, and sends parameter changes as UDP datagrams.

### Slots

The bus has one more slot than there are pedals. **Slot 0** holds the raw input written by the engine. Each pedal reads slot *N* and writes slot *N+1*. With 8 pedals, the processed signal ends up in **slot 8**, which the engine sends to the ALSA output. Reordering pedals changes which slots they read from and write to.

---

## 🐳 Docker Requirements

If you write your own compose file or run containers manually, every container on the bus needs:

- **Access to the sound device** (engine): pass `/dev/snd` into the container, e.g. `devices: ["/dev/snd:/dev/snd"]`.
- **A shared `/dev/shm`**: all engine and plugin containers must see the same shared memory, e.g. with `ipc: host` or a shared IPC namespace.
- **Docker socket** (dashboard only): mounted so the dashboard can discover running pedals. See the security note below.

---

## 🔒 Security Note

The dashboard mounts the Docker socket (`/var/run/docker.sock`), which is effectively **root access to the host**. The web UI has no authentication. Run it only on a trusted local network, and **do not expose port 80 to the internet**. If you need remote access, put it behind a VPN or an authenticated reverse proxy.

---

## 🧩 Writing Your Own Plugin

A plugin is a separate container with a C++17 DSP implementation. At a high level it must:

1. Attach to the `/pedal_bus` shared memory segment.
2. Wait on the semaphore for its input slot.
3. Process one audio block.
4. Write the result to the next slot and post that slot's semaphore.
5. Listen for parameter updates over UDP.

<!-- TODO: add a minimal plugin example (a skeleton .cpp file or a link to an existing plugin such as plugins/<name>) and a Dockerfile snippet. -->

---

## 🩺 Troubleshooting

**No sound**
- Check the device name with `arecord -l` and `aplay -l`, and make sure `AUDIO_DEVICE` matches it.
- Confirm the engine container can see `/dev/snd` (`docker exec <engine> ls /dev/snd`).
- Make sure the plugin containers share the same `/dev/shm` as the engine.

**Crackling, dropouts or xruns**
- Reduce the number of active pedals or the load on the host.
- Make sure nothing else is using the audio device.
- Check CPU load with `top`; on small boards the heavier effects (reverb, shimmer) cost the most.

**A pedal does not show up in the dashboard**
- Check that its container is running (`docker ps`).
- Check that the dashboard has access to the Docker socket.

**The dashboard does not open**
- Verify the device IP and that nothing else is using port 80.

---

## 📜 License

MIT License. Feel free to fork, add plugins, and build your own hardware pedalboards!
