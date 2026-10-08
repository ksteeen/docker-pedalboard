FROM alpine:latest AS builder
RUN apk add --no-cache g++ make alsa-lib-dev
WORKDIR /app
COPY include/ ./include/
COPY engine/engine.cpp ./engine/
RUN g++ -O3 -std=c++17 engine/engine.cpp -o /engine -lasound -lpthread

FROM alpine:latest
RUN apk add --no-cache alsa-lib libstdc++
COPY --from=builder /engine /usr/local/bin/engine
COPY engine/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
