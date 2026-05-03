#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace SimHeap {

enum class Profile {
  Desktop,
  X3,
  X4
};

void initializeFromEnvironment();
void setProfile(Profile profile);
Profile currentProfile();

void setHeapSize(uint32_t bytes);
void setBaseFreeHeap(uint32_t bytes);
void setLargestBlock(uint32_t bytes);

void setStaConnected(bool connected);
void setApEnabled(bool enabled);
void setScanActive(bool active);

uint32_t heapSize();
uint32_t freeHeap();
uint32_t minFreeHeap();
uint32_t largestFreeBlock();
uint32_t maxAllocHeap();
const char* profileName(Profile profile);

std::string summary();

}  // namespace SimHeap
