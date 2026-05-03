#pragma once

#include <cstdint>
#include "sim_heap.h"

inline uint32_t esp_get_free_heap_size() { return SimHeap::freeHeap(); }
