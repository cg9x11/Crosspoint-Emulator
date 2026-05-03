#pragma once

// Call before setup() so HalDisplay::begin() can use the window.
bool sim_display_init(void);
void sim_display_shutdown(void);
// Process SDL events (keyboard, etc.). Returns false if user requested quit.
bool sim_display_pump_events(void);
bool sim_display_save_screenshot(const char* path);
bool sim_display_toolbar_hit_test(int x, int y, uint8_t& outButton);
void sim_display_set_toolbar_state(uint8_t buttonMask);
