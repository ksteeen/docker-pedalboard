FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/overdrive/overdrive.cpp ./plugins/overdrive/
RUN g++ -O3 -std=c++17 plugins/overdrive/overdrive.cpp -o /overdrive -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Obsessive Drive"
LABEL pedal.category="distortion"
LABEL pedal.control_port="9003"
LABEL pedal.order="1"
LABEL pedal.controls='[ \
  {"id": "drive", "name": "Drive", "min": 0.05, "max": 1.0, "step": 0.05, "default": 0.45}, \
  {"id": "tone", "name": "Tone", "min": 0.1, "max": 0.95, "step": 0.05, "default": 0.50}, \
  {"id": "level", "name": "Level", "min": 0.1, "max": 1.0, "step": 0.05, "default": 0.60}, \
  {"id": "mode", "name": "HP/LP", "min": 0.0, "max": 1.0, "step": 1.0, "default": 1.0} \
]'

COPY --from=builder /overdrive /usr/local/bin/overdrive
ENTRYPOINT ["overdrive"]
