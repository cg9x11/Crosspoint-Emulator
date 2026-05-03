#pragma once

#include "WString.h"
#include "WiFiClient.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum HTTPMethod {
  HTTP_ANY,
  HTTP_GET,
  HTTP_POST,
  HTTP_DELETE,
  HTTP_PUT,
  HTTP_PATCH,
  HTTP_HEAD,
  HTTP_OPTIONS,
  HTTP_PROPFIND,
  HTTP_MKCOL,
  HTTP_MOVE,
  HTTP_COPY,
  HTTP_LOCK,
  HTTP_UNLOCK
};

constexpr int CONTENT_LENGTH_UNKNOWN = -1;

enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };

struct HTTPRaw {
  uint8_t* buf = nullptr;
  size_t size = 0;
  size_t currentSize = 0;
  size_t totalSize = 0;
  bool isFirst = false;
  bool isFinal = false;
};

struct HTTPUpload {
  int status = UPLOAD_FILE_ABORTED;
  String filename;
  String name;
  String type;
  size_t size = 0;
  size_t totalSize = 0;
  size_t currentSize = 0;
  const uint8_t* buf = nullptr;
};

class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  virtual bool canHandle(class WebServer& server, HTTPMethod method, const String& uri) {
    (void)server;
    (void)method;
    (void)uri;
    return false;
  }
  virtual bool canRaw(class WebServer& server, const String& uri) {
    (void)server;
    (void)uri;
    return false;
  }
  virtual void raw(class WebServer& server, const String& uri, HTTPRaw& raw) {
    (void)server;
    (void)uri;
    (void)raw;
  }
  virtual bool handle(class WebServer& server, HTTPMethod method, const String& uri) {
    (void)server;
    (void)method;
    (void)uri;
    return false;
  }
};

class WebServer {
 public:
  explicit WebServer(uint16_t port);
  ~WebServer();

  void on(const char* path, HTTPMethod method, std::function<void()> fn);
  void on(const char* path, HTTPMethod method, std::function<void()> fn, std::function<void()> uploadFn);
  void onNotFound(std::function<void()> fn);

  void begin();
  void stop();
  void handleClient();

  void addHandler(RequestHandler* handler);
  void collectHeaders(const char*[], size_t);

  void send(int code, const char* type, const char* content);
  void send(int code, const char* type, const String& content);
  void send(int code);
  void sendContent(const char* content);
  void sendContent(const String& content);
  void send_P(int code, const char* type, const char* content, size_t len);
  void sendHeader(const char* name, const char* value, bool first = false);
  void sendHeader(const String& name, const String& value, bool first = false);
  void setContentLength(size_t len);

  WiFiClient client();
  bool hasArg(const char* name) const;
  String arg(const char* name) const;
  String uri() const;
  String header(const char* name) const;
  HTTPMethod method() const;
  size_t clientContentLength() const;
  const HTTPUpload& upload() const;

  static String urlDecode(const String& value);

 private:
  struct Route {
    String path;
    HTTPMethod method = HTTP_ANY;
    std::function<void()> handler;
    std::function<void()> uploadHandler;
  };

  struct RequestContext {
    HTTPMethod method = HTTP_GET;
    String uri;
    std::map<std::string, String> args;
    std::map<std::string, String> headers;
    std::string body;
    size_t contentLength = 0;
  };

  struct ResponseContext {
    std::vector<std::pair<String, String>> headers;
    std::shared_ptr<WiFiClientState> clientState;
    long long contentLengthMode = -2;  // -2 unset, -1 chunked, >=0 fixed length
    bool headersSent = false;
    bool chunked = false;
    bool chunkedEnded = false;
  };

  class Impl;

  void clearRequestContext();
  void clearResponseContext();
  void sendHeadersIfNeeded(int code, const char* type, size_t bodyLength, bool chunked);
  void writeResponseBody(const uint8_t* data, size_t size);
  void maybeFinalizeChunkedResponse();
  bool dispatchRequest(const std::string& rawRequest, const std::shared_ptr<WiFiClientState>& clientState);
  bool dispatchRoute(const Route& route);
  bool dispatchCustomHandler();
  bool runMultipartUpload(const Route& route);

  uint16_t port_;
  HTTPUpload upload_;
  std::vector<Route> routes_;
  std::function<void()> notFoundHandler_;
  std::vector<std::unique_ptr<RequestHandler>> handlers_;
  std::unique_ptr<Impl> impl_;
  RequestContext currentRequest_;
  ResponseContext currentResponse_;
};
