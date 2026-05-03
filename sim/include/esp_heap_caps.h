#pragma once

#include <cstdint>

#define MALLOC_CAP_8BIT 0x0001

uint32_t heap_caps_get_largest_free_block(uint32_t caps);
