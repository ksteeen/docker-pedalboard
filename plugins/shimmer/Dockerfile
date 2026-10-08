FROM alpine:latest AS builder
RUN apk add --no-cache g++ make
WORKDIR /app
COPY include/ ./include/
COPY plugins/shimmer/shimmer.cpp ./plugins/shimmer/
RUN g++ -O3 -std=c++17 plugins/shimmer/shimmer.cpp -o /shimmer -lpthread

FROM alpine:latest
RUN apk add --no-cache libstdc++

LABEL pedal.name="Celestial Shimmer"
LABEL pedal.category="reverb"
LABEL pedal.control_port="9002"
LABEL pedal.order="7"
LABEL pedal.controls='[ \
  {"id": "mix", "name": "Mix", "min": 0.1, "max": 0.7, "step": 0.02, "default": 0.40}, \
  {"id": "decay", "name": "Decay", "min": 0.5, "max": 0.85, "step": 0.02, "default": 0.76}, \
  {"id": "shimmer", "name": "Shimmer", "min": 0.1, "max": 0.7, "step": 0.02, "default": 0.45} \
]'

COPY --from=builder /shimmer /usr/local/bin/shimmer
ENTRYPOINT ["shimmer"]
