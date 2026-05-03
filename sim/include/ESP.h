#pragma once

#include <cstdint>
#include "sim_heap.h"

class ESPClass {
 public:
  uint32_t getFreeHeap() const { return SimHeap::freeHeap(); }
  uint32_t getHeapSize() const { return SimHeap::heapSize(); }
  uint32_t getMinFreeHeap() const { return SimHeap::minFreeHeap(); }
  uint32_t getMaxAllocHeap() const { return SimHeap::maxAllocHeap(); }
  void restart() { /* no-op in sim */ }
};

extern ESPClass ESP;
