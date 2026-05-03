#include "sim_heap.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace SimHeap {
namespace {

struct State {
  Profile profile = Profile::X3;
  uint32_t heapSize = 321296;
  uint32_t baseFreeHeap = 96576;
  uint32_t baseLargestBlock = 73716;
  uint32_t staFreePenalty = 16748;
  uint32_t apFreePenalty = 85476;
  uint32_t scanFreePenalty = 12000;
  uint32_t staLargestPenalty = 0;
  uint32_t apLargestPenalty = 65524;
  uint32_t scanLargestPenalty = 18000;
  bool staConnected = true;
  bool apEnabled = false;
  bool scanActive = false;
  uint32_t minObservedFreeHeap = 0;
};

State g_state;
std::mutex g_mutex;
bool g_initialized = false;

uint32_t currentFreePenaltyLocked() {
  uint32_t penalty = 0;
  if (g_state.staConnected) {
    penalty += g_state.staFreePenalty;
  }
  if (g_state.apEnabled) {
    penalty += g_state.apFreePenalty;
  }
  if (g_state.scanActive) {
    penalty += g_state.scanFreePenalty;
  }
  return penalty;
}

uint32_t currentFreeHeapLocked() {
  const uint32_t penalty = currentFreePenaltyLocked();
  return g_state.baseFreeHeap > penalty ? g_state.baseFreeHeap - penalty : 0;
}

uint32_t currentLargestPenaltyLocked() {
  uint32_t penalty = 0;
  if (g_state.staConnected) {
    penalty += g_state.staLargestPenalty;
  }
  if (g_state.apEnabled) {
    penalty += g_state.apLargestPenalty;
  }
  if (g_state.scanActive) {
    penalty += g_state.scanLargestPenalty;
  }
  return penalty;
}

uint32_t currentLargestLocked() {
  const uint32_t freeHeapValue = currentFreeHeapLocked();
  const uint32_t largestPenalty = currentLargestPenaltyLocked();
  const uint32_t largestBlock =
      g_state.baseLargestBlock > largestPenalty ? g_state.baseLargestBlock - largestPenalty : 0;
  return std::min(largestBlock, freeHeapValue);
}

void updateMinFreeHeapLocked() {
  const uint32_t current = currentFreeHeapLocked();
  if (g_state.minObservedFreeHeap == 0 || current < g_state.minObservedFreeHeap) {
    g_state.minObservedFreeHeap = current;
  }
}

uint32_t readEnvU32(const char* key, uint32_t fallback) {
  const char* raw = std::getenv(key);
  if (!raw || !*raw) return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw, &end, 10);
  if (end == raw || *end != '\0') return fallback;
  return static_cast<uint32_t>(parsed);
}

void applyProfileLocked(Profile profile) {
  g_state.profile = profile;
  if (profile == Profile::Desktop) {
    g_state.heapSize = 16 * 1024 * 1024;
    g_state.baseFreeHeap = 8 * 1024 * 1024;
    g_state.baseLargestBlock = 4 * 1024 * 1024;
    g_state.staFreePenalty = 0;
    g_state.apFreePenalty = 0;
    g_state.scanFreePenalty = 0;
    g_state.staLargestPenalty = 0;
    g_state.apLargestPenalty = 0;
    g_state.scanLargestPenalty = 0;
  } else if (profile == Profile::X4) {
    g_state.heapSize = 321296;
    g_state.baseFreeHeap = 104768;
    g_state.baseLargestBlock = 77824;
    g_state.staFreePenalty = 16384;
    g_state.apFreePenalty = 77824;
    g_state.scanFreePenalty = 10240;
    g_state.staLargestPenalty = 1024;
    g_state.apLargestPenalty = 61440;
    g_state.scanLargestPenalty = 12288;
  } else {
    g_state.heapSize = 321296;
    g_state.baseFreeHeap = 96576;
    g_state.baseLargestBlock = 73716;
    g_state.staFreePenalty = 16748;
    g_state.apFreePenalty = 85476;
    g_state.scanFreePenalty = 12000;
    g_state.staLargestPenalty = 0;
    g_state.apLargestPenalty = 65524;
    g_state.scanLargestPenalty = 18000;
  }
  g_state.minObservedFreeHeap = 0;
  updateMinFreeHeapLocked();
}

}  // namespace

void initializeFromEnvironment() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_initialized) return;
  g_initialized = true;

  const char* profileRaw = std::getenv("SIM_HEAP_PROFILE");
  if (profileRaw && std::string(profileRaw) == "desktop") {
    applyProfileLocked(Profile::Desktop);
  } else if (profileRaw && std::string(profileRaw) == "x4") {
    applyProfileLocked(Profile::X4);
  } else {
    const char* deviceRaw = std::getenv("SIM_DEVICE");
    if (profileRaw && std::string(profileRaw) == "x3") {
      applyProfileLocked(Profile::X3);
    } else if (deviceRaw && std::string(deviceRaw) == "x4") {
      applyProfileLocked(Profile::X4);
    } else {
      applyProfileLocked(Profile::X3);
    }
  }

  g_state.heapSize = readEnvU32("SIM_HEAP_SIZE", g_state.heapSize);
  g_state.baseFreeHeap = readEnvU32("SIM_HEAP_FREE", g_state.baseFreeHeap);
  g_state.baseLargestBlock = readEnvU32("SIM_HEAP_LARGEST", g_state.baseLargestBlock);
  g_state.staFreePenalty = readEnvU32("SIM_HEAP_STA_PENALTY", g_state.staFreePenalty);
  g_state.apFreePenalty = readEnvU32("SIM_HEAP_AP_PENALTY", g_state.apFreePenalty);
  g_state.scanFreePenalty = readEnvU32("SIM_HEAP_SCAN_PENALTY", g_state.scanFreePenalty);
  g_state.staLargestPenalty = readEnvU32("SIM_HEAP_STA_LARGEST_PENALTY", g_state.staLargestPenalty);
  g_state.apLargestPenalty = readEnvU32("SIM_HEAP_AP_LARGEST_PENALTY", g_state.apLargestPenalty);
  g_state.scanLargestPenalty = readEnvU32("SIM_HEAP_SCAN_LARGEST_PENALTY", g_state.scanLargestPenalty);
  g_state.minObservedFreeHeap = 0;
  updateMinFreeHeapLocked();
}

void setProfile(Profile profile) {
  std::lock_guard<std::mutex> lock(g_mutex);
  applyProfileLocked(profile);
}

Profile currentProfile() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.profile;
}

void setHeapSize(uint32_t bytes) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.heapSize = bytes;
}

void setBaseFreeHeap(uint32_t bytes) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.baseFreeHeap = bytes;
  updateMinFreeHeapLocked();
}

void setLargestBlock(uint32_t bytes) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.baseLargestBlock = bytes;
}

void setStaConnected(bool connected) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.staConnected = connected;
  updateMinFreeHeapLocked();
}

void setApEnabled(bool enabled) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.apEnabled = enabled;
  updateMinFreeHeapLocked();
}

void setScanActive(bool active) {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.scanActive = active;
  updateMinFreeHeapLocked();
}

uint32_t heapSize() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state.heapSize;
}

uint32_t freeHeap() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  updateMinFreeHeapLocked();
  return currentFreeHeapLocked();
}

uint32_t minFreeHeap() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  updateMinFreeHeapLocked();
  return g_state.minObservedFreeHeap;
}

uint32_t largestFreeBlock() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  return currentLargestLocked();
}

uint32_t maxAllocHeap() { return largestFreeBlock(); }

const char* profileName(Profile profile) {
  switch (profile) {
    case Profile::Desktop: return "desktop";
    case Profile::X3: return "x3";
    case Profile::X4: return "x4";
  }
  return "unknown";
}

std::string summary() {
  initializeFromEnvironment();
  std::lock_guard<std::mutex> lock(g_mutex);
  return "heap=" + std::to_string(currentFreeHeapLocked()) + ", largest=" + std::to_string(currentLargestLocked()) +
         ", min=" + std::to_string(g_state.minObservedFreeHeap) + ", total=" + std::to_string(g_state.heapSize);
}

}  // namespace SimHeap

uint32_t heap_caps_get_largest_free_block(uint32_t) { return SimHeap::largestFreeBlock(); }
