FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/vibrato/vibrato.cpp ./plugins/vibrato/
RUN g++ -O3 -std=c++17 plugins/vibrato/vibrato.cpp -o /vibrato -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Tape Vibrato"
LABEL pedal.category="mod"
LABEL pedal.control_port="9004"
LABEL pedal.order="4"
LABEL pedal.controls='[ \
  {"id": "rate", "name": "Speed", "min": 0.3, "max": 7.0, "step": 0.1, "default": 2.5}, \
  {"id": "depth", "name": "Depth", "min": 0.0, "max": 1.0, "step": 0.05, "default": 0.45}, \
  {"id": "flutter", "name": "Flutter", "min": 0.0, "max": 1.0, "step": 0.05, "default": 0.35}, \
  {"id": "blend", "name": "Blend", "min": 0.2, "max": 1.0, "step": 0.05, "default": 1.0} \
]'

COPY --from=builder /vibrato /usr/local/bin/vibrato
ENTRYPOINT ["vibrato"]
