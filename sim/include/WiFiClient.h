#pragma once

#include "ArduinoStub.h"

#include <cstdint>
#include <memory>

struct WiFiClientState;

class WiFiClient : public Stream {
 public:
  WiFiClient();
  explicit WiFiClient(std::shared_ptr<WiFiClientState> state);

  int connect(const char* host, uint16_t port);
  void stop();
  bool connected();
  void clear();

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t size) override;
  size_t write(Stream& stream);

 private:
  std::shared_ptr<WiFiClientState> state_;
};

using NetworkClient = WiFiClient;
