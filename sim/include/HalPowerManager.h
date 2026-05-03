#pragma once

#include "Arduino.h"
#include "HalGPIO.h"

void emulatorWakeFromSleep();

class HalPowerManager {
 public:
  static constexpr int LOW_POWER_FREQ = 10;
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;
  static constexpr unsigned long BATTERY_POLL_MS = 1500;

  void begin() {}
  void setPowerSaving(bool) {}
  void startDeepSleep(HalGPIO& gpio) const {
    // Emulator behavior: block in a lightweight sleep loop until power is released,
    // then wake on the next power press and return to Home.
    while (gpio.isPressed(HalGPIO::BTN_POWER)) {
      gpio.update();
      delay(10);
    }

    while (true) {
      gpio.update();
      if (gpio.wasPressed(HalGPIO::BTN_POWER) || gpio.isPressed(HalGPIO::BTN_POWER)) {
        break;
      }
      delay(10);
    }

    while (gpio.isPressed(HalGPIO::BTN_POWER)) {
      gpio.update();
      delay(10);
    }

    emulatorWakeFromSleep();
  }
  uint16_t getBatteryPercentage() const { return 100; }

  class Lock {
   public:
    Lock() = default;
    ~Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};

inline HalPowerManager powerManager;
