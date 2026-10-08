FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/cabsim/cabsim.cpp ./plugins/cabsim/
RUN g++ -O3 -std=c++17 plugins/cabsim/cabsim.cpp -o /cabsim -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Cab Sim DI"
LABEL pedal.category="default"
LABEL pedal.control_port="9008"
LABEL pedal.order="8"
LABEL pedal.controls='[ \
  {"id": "type", "name": "Cab Type (0=US, 1=UK)", "min": 0.0, "max": 1.0, "step": 1.0, "default": 1.0}, \
  {"id": "presence", "name": "Presence", "min": 0.0, "max": 1.0, "step": 0.05, "default": 0.50}, \
  {"id": "volume", "name": "Volume", "min": 0.2, "max": 1.2, "step": 0.05, "default": 0.85} \
]'

COPY --from=builder /cabsim /usr/local/bin/cabsim
ENTRYPOINT ["cabsim"]
