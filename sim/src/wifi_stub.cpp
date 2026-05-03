#include "WiFi.h"
#include "sim_heap.h"

void WiFiClass::mode(int modeValue) {
  mode_ = static_cast<wifi_mode_t>(modeValue);
  if (mode_ == WIFI_MODE_AP) {
    status_ = WL_DISCONNECTED;
    SimHeap::setStaConnected(false);
  } else if (mode_ == WIFI_MODE_STA) {
    if (!apEnabled_) {
      status_ = WL_CONNECTED;
      SimHeap::setStaConnected(true);
    }
  }
}

void WiFiClass::disconnect(bool, bool) {
  status_ = WL_DISCONNECTED;
  localIp_ = IPAddress(0, 0, 0, 0);
  currentSsid_ = String("");
  SimHeap::setStaConnected(false);
}

void WiFiClass::softAPdisconnect(bool) {
  apEnabled_ = false;
  SimHeap::setApEnabled(false);
}

bool WiFiClass::softAP(const char* ssid, const char*, int, bool, int) {
  apEnabled_ = true;
  currentSsid_ = ssid ? String(ssid) : String("Crosspoint-Reader");
  SimHeap::setApEnabled(true);
  return true;
}

void WiFiClass::scanDelete() {
  scanActive_ = false;
  SimHeap::setScanActive(false);
}

int16_t WiFiClass::scanNetworks(bool async) {
  scanActive_ = async;
  SimHeap::setScanActive(async);
  return async ? WIFI_SCAN_RUNNING : static_cast<int16_t>(scanResults_.size());
}

int16_t WiFiClass::scanComplete() {
  if (!scanActive_) {
    return static_cast<int16_t>(scanResults_.size());
  }
  scanActive_ = false;
  SimHeap::setScanActive(false);
  return static_cast<int16_t>(scanResults_.size());
}

bool WiFiClass::begin(const char* ssid, const char*) {
  mode_ = WIFI_MODE_STA;
  status_ = WL_CONNECTED;
  localIp_ = IPAddress(127, 0, 0, 1);
  currentSsid_ = ssid ? String(ssid) : String("Crosspoint-Emulator");
  SimHeap::setStaConnected(true);
  return true;
}

WiFiClass WiFi;
