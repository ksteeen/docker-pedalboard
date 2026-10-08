FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/flanger/flanger.cpp ./plugins/flanger/
RUN g++ -O3 -std=c++17 plugins/flanger/flanger.cpp -o /flanger -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Jet Flanger"
LABEL pedal.category="mod"
LABEL pedal.control_port="9007"
LABEL pedal.order="4"
LABEL pedal.controls='[ \
  {"id": "rate", "name": "Speed", "min": 0.05, "max": 3.5, "step": 0.05, "default": 0.35}, \
  {"id": "depth", "name": "Depth", "min": 0.0, "max": 1.0, "step": 0.05, "default": 0.70}, \
  {"id": "regen", "name": "Resonance", "min": 0.0, "max": 0.90, "step": 0.02, "default": 0.65}, \
  {"id": "manual", "name": "Manual", "min": 0.0, "max": 1.0, "step": 0.05, "default": 0.40} \
]'

COPY --from=builder /flanger /usr/local/bin/flanger
ENTRYPOINT ["flanger"]
