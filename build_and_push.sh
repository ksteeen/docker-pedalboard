#!/bin/bash
set -e

# Вкажіть ваш логін на Docker Hub
DOCKER_USER="ksteeen"
TAG="latest"
PLATFORMS="linux/amd64,linux/arm64"

echo "=== Початок збірки образів для $DOCKER_USER під $PLATFORMS ==="

# 1. Engine
echo "--> Збірка Engine..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-engine:$TAG \
  -f engine/Dockerfile --push .

# 2. Dashboard
echo "--> Збірка Dashboard..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-dashboard:$TAG \
  -f dashboard/Dockerfile --push .

# 3. Overdrive
echo "--> Збірка Overdrive..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-overdrive:$TAG \
  -f plugins/overdrive/Dockerfile --push .

# 4. Fuzz Face
echo "--> Збірка Fuzz Face..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-fuzz:$TAG \
  -f plugins/fuzz/Dockerfile --push .

# 5. Jet Flanger
echo "--> Збірка Jet Flanger..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-flanger:$TAG \
  -f plugins/flanger/Dockerfile --push .

# 6. Tape Vibrato
echo "--> Збірка Tape Vibrato..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-vibrato:$TAG \
  -f plugins/vibrato/Dockerfile --push .

# 7. Echo Tape Delay
echo "--> Збірка Echo Tape Delay..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-delay:$TAG \
  -f plugins/delay/Dockerfile --push .

# 8. Spring Reverb
echo "--> Збірка Spring Reverb..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-reverb:$TAG \
  -f plugins/reverb/Dockerfile --push .

# 9. Celestial Shimmer
echo "--> Збірка Celestial Shimmer..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-shimmer:$TAG \
  -f plugins/shimmer/Dockerfile --push .

# 10. Cab Sim DI
echo "--> Збірка Cab Sim DI..."
docker buildx build --platform $PLATFORMS \
  -t $DOCKER_USER/pedal-cabsim:$TAG \
  -f plugins/cabsim/Dockerfile --push .

echo "=== Усі образи успішно скомпільовані та запушені в Docker Hub! ==="
