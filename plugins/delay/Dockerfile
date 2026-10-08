FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/delay/delay.cpp ./plugins/delay/
RUN g++ -O3 -std=c++17 plugins/delay/delay.cpp -o /delay -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Echo Tape Delay"
LABEL pedal.category="delay"
LABEL pedal.control_port="9005"
LABEL pedal.order="5"
LABEL pedal.controls='[ \
  {"id": "time", "name": "Time (ms)", "min": 50.0, "max": 900.0, "step": 10.0, "default": 350.0}, \
  {"id": "feedback", "name": "Repeats", "min": 0.0, "max": 0.90, "step": 0.02, "default": 0.45}, \
  {"id": "mix", "name": "Mix", "min": 0.0, "max": 0.85, "step": 0.05, "default": 0.35}, \
  {"id": "tone", "name": "Warmth", "min": 0.1, "max": 0.90, "step": 0.05, "default": 0.40} \
]'

COPY --from=builder /delay /usr/local/bin/delay
ENTRYPOINT ["delay"]
