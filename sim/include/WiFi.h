#pragma once

#include "WString.h"
#include <cstdint>
#include <vector>

class IPAddress {
 public:
  IPAddress() : a_(0), b_(0), c_(0), d_(0) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : a_(a), b_(b), c_(c), d_(d) {}
  uint8_t operator[](int i) const {
    if (i == 0) return a_;
    if (i == 1) return b_;
    if (i == 2) return c_;
    if (i == 3) return d_;
    return 0;
  }
  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a_, b_, c_, d_);
    return String(buf);
  }
  bool operator!=(const IPAddress& o) const {
    return a_ != o.a_ || b_ != o.b_ || c_ != o.c_ || d_ != o.d_;
  }
  bool operator==(const IPAddress& o) const {
    return a_ == o.a_ && b_ == o.b_ && c_ == o.c_ && d_ == o.d_;
  }

 private:
  uint8_t a_, b_, c_, d_;
};

enum wl_status_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL,
  WL_SCAN_COMPLETED,
  WL_CONNECTED,
  WL_CONNECT_FAILED,
  WL_CONNECTION_LOST,
  WL_DISCONNECTED
};

enum { WIFI_OFF, WIFI_STA, WIFI_AP };
enum wifi_mode_t { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 };
enum { WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2 };
enum { WIFI_AUTH_OPEN = 0 };

class WiFiClass {
 public:
  void mode(int mode);
  void persistent(bool) {}
  wl_status_t status() const { return status_; }
  void disconnect(bool = true, bool = false);
  void softAPdisconnect(bool = true);
  bool softAP(const char* ssid, const char* pass = nullptr, int = 1, bool = false, int = 4);
  IPAddress softAPIP() const { return apIp_; }
  String softAPSSID() const { return apEnabled_ ? currentSsid_ : String(""); }
  IPAddress localIP() const { return localIp_; }
  String SSID() const { return currentSsid_; }
  String SSID(int i) const {
    if (i < 0 || static_cast<size_t>(i) >= scanResults_.size()) return String("");
    return scanResults_[static_cast<size_t>(i)].ssid;
  }
  wifi_mode_t getMode() const { return mode_; }
  String getHostname() const { return String("crosspoint-emulator"); }
  bool setHostname(const char*) { return true; }
  void setSleep(bool) {}
  int softAPgetStationNum() const { return apEnabled_ ? 1 : 0; }
  void macAddress(uint8_t* mac) {
    for (int i = 0; i < 6; i++) mac[i] = 0;
  }
  String macAddress() const { return String("00:00:00:00:00:00"); }
  void scanDelete();
  int16_t scanNetworks(bool async = false);
  int16_t scanComplete();
  int32_t RSSI() const { return -48; }
  int32_t RSSI(int i) const {
    if (i < 0 || static_cast<size_t>(i) >= scanResults_.size()) return -100;
    return scanResults_[static_cast<size_t>(i)].rssi;
  }
  int encryptionType(int i) const {
    if (i < 0 || static_cast<size_t>(i) >= scanResults_.size()) return WIFI_AUTH_OPEN;
    return scanResults_[static_cast<size_t>(i)].encrypted ? 1 : WIFI_AUTH_OPEN;
  }
  int32_t channel(int i) const {
    if (i < 0 || static_cast<size_t>(i) >= scanResults_.size()) return 1;
    return scanResults_[static_cast<size_t>(i)].channel;
  }
  bool begin(const char* ssid, const char* pass = nullptr);

 private:
  struct ScanResult {
    String ssid;
    int32_t rssi = -60;
    bool encrypted = false;
    int32_t channel = 1;
  };

  wifi_mode_t mode_ = WIFI_MODE_STA;
  wl_status_t status_ = WL_CONNECTED;
  bool apEnabled_ = false;
  bool scanActive_ = false;
  IPAddress apIp_ = IPAddress(192, 168, 4, 1);
  IPAddress localIp_ = IPAddress(127, 0, 0, 1);
  String currentSsid_ = String("Crosspoint-Emulator");
  std::vector<ScanResult> scanResults_ = {
      {String("Crosspoint-Emulator"), -48, false, 1},
      {String("Office-5G"), -63, true, 149},
      {String("Cafe-Wifi"), -72, false, 6},
  };
};

extern WiFiClass WiFi;
