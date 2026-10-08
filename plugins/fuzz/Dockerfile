FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/fuzz/fuzz.cpp ./plugins/fuzz/
RUN g++ -O3 -std=c++17 plugins/fuzz/fuzz.cpp -o /fuzz -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Fuzz Face"
LABEL pedal.category="distortion"
LABEL pedal.control_port="9000"
LABEL pedal.order="2"
LABEL pedal.controls='[ \
  {"id": "fuzz", "name": "Fuzz", "min": 1.0, "max": 25.0, "step": 0.5, "default": 12.0}, \
  {"id": "tone", "name": "Tone", "min": 0.1, "max": 0.8, "step": 0.05, "default": 0.35}, \
  {"id": "level", "name": "Level", "min": 0.1, "max": 1.0, "step": 0.05, "default": 0.5} \
]'

COPY --from=builder /fuzz /usr/local/bin/fuzz
ENTRYPOINT ["fuzz"]
