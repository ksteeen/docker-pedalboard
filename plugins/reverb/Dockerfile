FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/reverb/reverb.cpp ./plugins/reverb/
RUN g++ -O3 -std=c++17 plugins/reverb/reverb.cpp -o /reverb -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Spring Reverb"
LABEL pedal.category="reverb"
LABEL pedal.control_port="9001"
LABEL pedal.order="6"
LABEL pedal.controls='[ \
  {"id": "mix", "name": "Mix", "min": 0.0, "max": 0.85, "step": 0.05, "default": 0.35}, \
  {"id": "decay", "name": "Decay", "min": 0.2, "max": 0.92, "step": 0.02, "default": 0.75}, \
  {"id": "tone", "name": "Tone", "min": 0.05, "max": 0.6, "step": 0.05, "default": 0.28} \
]'

COPY --from=builder /reverb /usr/local/bin/reverb
ENTRYPOINT ["reverb"]
