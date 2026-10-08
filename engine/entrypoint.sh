#!/bin/sh

cleanup() {
    echo "[Engine] Видалення /dev/shm/pedal_bus при зупинці..."
    rm -f /dev/shm/pedal_bus
    exit 0
}

trap cleanup SIGTERM SIGINT

rm -f /dev/shm/pedal_bus

/usr/local/bin/engine "$@" &
PID=$!
wait $PID
