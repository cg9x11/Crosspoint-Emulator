#pragma once

#include "WString.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class BLEUUID {
 public:
  explicit BLEUUID(const char* value = "") : value_(value ? value : "") {}

 private:
  String value_;
};

class BLEAddress {
 public:
  BLEAddress(const String& value = String("00:00:00:00:00:00"), uint8_t type = 0) : value_(value), type_(type) {}
  String toString() const { return value_; }
  uint8_t getType() const { return type_; }

 private:
  String value_;
  uint8_t type_ = 0;
};

class BLEAdvertisedDevice {
 public:
  BLEAdvertisedDevice() = default;
  BLEAdvertisedDevice(String name, String address, int rssi, bool connectable = true, uint8_t addressType = 0)
      : name_(std::move(name)),
        address_(std::move(address)),
        rssi_(rssi),
        connectable_(connectable),
        addressType_(addressType) {}

  bool haveName() const { return !name_.isEmpty(); }
  String getName() const { return name_; }
  bool haveServiceUUID() const { return true; }
  bool isAdvertisingService(const BLEUUID&) const { return true; }
  bool isConnectable() const { return connectable_; }
  bool haveRSSI() const { return true; }
  int getRSSI() const { return rssi_; }
  BLEAddress getAddress() const { return BLEAddress(address_, addressType_); }
  uint8_t getAddressType() const { return addressType_; }

 private:
  String name_ = String("TapXR");
  String address_ = String("00:11:22:33:44:55");
  int rssi_ = -42;
  bool connectable_ = true;
  uint8_t addressType_ = 0;
};

class BLEScanResults {
 public:
  int getCount() const { return static_cast<int>(devices_.size()); }
  BLEAdvertisedDevice getDevice(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= devices_.size()) {
      return BLEAdvertisedDevice();
    }
    return devices_[static_cast<size_t>(index)];
  }

 private:
  std::vector<BLEAdvertisedDevice> devices_ = {
      BLEAdvertisedDevice(String("TapXR Emulator"), String("00:11:22:33:44:55"), -38, true, 0)};
};

class BLEScan {
 public:
  void setActiveScan(bool) {}
  void setInterval(int) {}
  void setWindow(int) {}
  BLEScanResults* start(uint32_t, bool) { return &results_; }
  void clearResults() {}

 private:
  BLEScanResults results_;
};

class BLERemoteCharacteristic {
 public:
  using NotifyCallback = void (*)(BLERemoteCharacteristic*, uint8_t*, size_t, bool);

  void registerForNotify(NotifyCallback callback) { callback_ = callback; }
  bool writeValue(const uint8_t*, size_t, bool) { return true; }

 private:
  NotifyCallback callback_ = nullptr;
};

class BLERemoteService {
 public:
  BLERemoteCharacteristic* getCharacteristic(const BLEUUID&) { return &characteristic_; }

 private:
  BLERemoteCharacteristic characteristic_;
};

class BLEClient {
 public:
  bool connect(const BLEAddress&, uint8_t, uint32_t) {
    connected_ = true;
    return true;
  }
  BLERemoteService* getService(const BLEUUID&) { return &service_; }
  bool isConnected() const { return connected_; }
  void disconnect() { connected_ = false; }

 private:
  bool connected_ = false;
  BLERemoteService service_;
};

class BLEDevice {
 public:
  static void init(const char*) {}
  static BLEScan* getScan() {
    static BLEScan scan;
    return &scan;
  }
  static BLEClient* createClient() { return new BLEClient(); }
  static void deinit(bool) {}
};
