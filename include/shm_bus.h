#pragma once
#include <semaphore.h>
#include <cstdint>
#include <cstddef>

constexpr size_t BLOCK_SIZE = 256;
constexpr size_t MAX_SLOTS = 10;

struct PedalBus {
    sem_t sem[MAX_SLOTS];
    float slots[MAX_SLOTS][BLOCK_SIZE];
};
