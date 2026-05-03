#include "WebServer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SimSocket = SOCKET;
constexpr SimSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SimSocket = int;
constexpr SimSocket kInvalidSocket = -1;
#endif

namespace {

#ifdef _WIN32
bool ensureSocketRuntime() {
  static const bool ready = []() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}
#else
bool ensureSocketRuntime() { return true; }
#endif

void closeSocket(const SimSocket socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

bool setSocketNonBlocking(const SimSocket socket) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool isWouldBlockError() {
#ifdef _WIN32
  const int code = WSAGetLastError();
  return code == WSAEWOULDBLOCK;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

bool isInterruptedError() {
#ifdef _WIN32
  return WSAGetLastError() == WSAEINTR;
#else
  return errno == EINTR;
#endif
}

bool socketSendAll(const SimSocket socket, const uint8_t* data, const size_t size) {
  size_t sent = 0;
  int stallCount = 0;
  while (sent < size) {
#ifdef _WIN32
    const int chunk = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
#else
    const ssize_t chunk = send(socket, data + sent, size - sent, 0);
#endif
    if (chunk > 0) {
      sent += static_cast<size_t>(chunk);
      stallCount = 0;
      continue;
    }
    if (chunk == 0) {
      return false;
    }
    if (isInterruptedError()) {
      continue;
    }
    if (isWouldBlockError()) {
      if (++stallCount > 5000) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    return false;
  }
  return true;
}

std::string toLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trimCopy(const std::string& value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}

std::string decodeUrl(const std::string& encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); i++) {
    const char c = encoded[i];
    if (c == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (c == '%' && i + 2 < encoded.size()) {
      const char hi = encoded[i + 1];
      const char lo = encoded[i + 2];
      auto hexValue = [](const char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
      };
      const int hiValue = hexValue(hi);
      const int loValue = hexValue(lo);
      if (hiValue >= 0 && loValue >= 0) {
        decoded.push_back(static_cast<char>((hiValue << 4) | loValue));
        i += 2;
        continue;
      }
    }
    decoded.push_back(c);
  }
  return decoded;
}

void parseUrlEncodedArgs(const std::string& source, std::map<std::string, String>& target) {
  size_t cursor = 0;
  while (cursor <= source.size()) {
    const size_t nextAmp = source.find('&', cursor);
    const std::string pair = source.substr(cursor, nextAmp == std::string::npos ? std::string::npos : nextAmp - cursor);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      const std::string key = decodeUrl(eq == std::string::npos ? pair : pair.substr(0, eq));
      const std::string value = decodeUrl(eq == std::string::npos ? "" : pair.substr(eq + 1));
      target[key] = String(value);
    }
    if (nextAmp == std::string::npos) {
      break;
    }
    cursor = nextAmp + 1;
  }
}

HTTPMethod parseMethodToken(const std::string& method) {
  if (method == "GET") return HTTP_GET;
  if (method == "POST") return HTTP_POST;
  if (method == "DELETE") return HTTP_DELETE;
  if (method == "PUT") return HTTP_PUT;
  if (method == "PATCH") return HTTP_PATCH;
  if (method == "HEAD") return HTTP_HEAD;
  if (method == "OPTIONS") return HTTP_OPTIONS;
  if (method == "PROPFIND") return HTTP_PROPFIND;
  if (method == "MKCOL") return HTTP_MKCOL;
  if (method == "MOVE") return HTTP_MOVE;
  if (method == "COPY") return HTTP_COPY;
  if (method == "LOCK") return HTTP_LOCK;
  if (method == "UNLOCK") return HTTP_UNLOCK;
  return HTTP_ANY;
}

const char* reasonPhrase(const int code) {
  switch (code) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 500:
      return "Internal Server Error";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    default:
      return "OK";
  }
}

bool headerContains(const std::map<std::string, String>& headers, const char* name, const char* expected) {
  const auto it = headers.find(toLowerCopy(name));
  if (it == headers.end()) {
    return false;
  }
  return toLowerCopy(it->second.str()).find(toLowerCopy(expected)) != std::string::npos;
}

std::string extractHeaderParam(const std::string& headerValue, const char* key) {
  const std::string needle = std::string(key) + "=";
  const size_t start = headerValue.find(needle);
  if (start == std::string::npos) {
    return "";
  }
  size_t valueStart = start + needle.size();
  if (valueStart >= headerValue.size()) {
    return "";
  }
  if (headerValue[valueStart] == '"') {
    valueStart++;
    const size_t endQuote = headerValue.find('"', valueStart);
    if (endQuote == std::string::npos) {
      return headerValue.substr(valueStart);
    }
    return headerValue.substr(valueStart, endQuote - valueStart);
  }
  size_t end = headerValue.find(';', valueStart);
  if (end == std::string::npos) {
    end = headerValue.size();
  }
  return trimCopy(headerValue.substr(valueStart, end - valueStart));
}

struct ParsedRequest {
  HTTPMethod method = HTTP_GET;
  std::string target;
  std::map<std::string, String> headers;
  std::string body;
  size_t contentLength = 0;
};

bool tryParseRequestBuffer(const std::string& rawRequest, ParsedRequest& parsed) {
  size_t headerEnd = rawRequest.find("\r\n\r\n");
  size_t delimiterLen = 4;
  if (headerEnd == std::string::npos) {
    headerEnd = rawRequest.find("\n\n");
    delimiterLen = 2;
  }
  if (headerEnd == std::string::npos) {
    return false;
  }

  std::string head = rawRequest.substr(0, headerEnd);
  std::istringstream stream(head);
  std::string line;
  if (!std::getline(stream, line)) {
    return false;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  std::istringstream requestLine(line);
  std::string methodToken;
  std::string target;
  std::string version;
  if (!(requestLine >> methodToken >> target >> version)) {
    return false;
  }

  parsed = ParsedRequest{};
  parsed.method = parseMethodToken(methodToken);
  parsed.target = target;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string name = toLowerCopy(trimCopy(line.substr(0, colon)));
    const std::string value = trimCopy(line.substr(colon + 1));
    parsed.headers[name] = String(value);
  }

  const auto contentLengthIt = parsed.headers.find("content-length");
  if (contentLengthIt != parsed.headers.end()) {
    try {
      parsed.contentLength = static_cast<size_t>(std::stoull(contentLengthIt->second.str()));
    } catch (...) {
      parsed.contentLength = 0;
    }
  }

  const size_t bodyStart = headerEnd + delimiterLen;
  if (rawRequest.size() < bodyStart + parsed.contentLength) {
    return false;
  }

  parsed.body = rawRequest.substr(bodyStart, parsed.contentLength);
  return true;
}

}  // namespace

struct WiFiClientState {
  SimSocket socket = kInvalidSocket;
  bool open = false;
};

WiFiClient::WiFiClient() = default;

WiFiClient::WiFiClient(std::shared_ptr<WiFiClientState> state) : state_(std::move(state)) {}

int WiFiClient::connect(const char* host, uint16_t port) {
  (void)host;
  (void)port;
  return 0;
}

void WiFiClient::stop() {
  if (!state_ || !state_->open) {
    return;
  }
  closeSocket(state_->socket);
  state_->socket = kInvalidSocket;
  state_->open = false;
}

bool WiFiClient::connected() { return state_ && state_->open && state_->socket != kInvalidSocket; }

void WiFiClient::clear() {}

size_t WiFiClient::write(uint8_t c) { return write(&c, 1); }

size_t WiFiClient::write(const uint8_t* buf, size_t size) {
  if (!connected() || !buf || size == 0) {
    return 0;
  }
  return socketSendAll(state_->socket, buf, size) ? size : 0;
}

size_t WiFiClient::write(Stream& stream) {
  size_t n = 0;
  while (stream.available()) {
    const int c = stream.read();
    if (c < 0) break;
    n += write(static_cast<uint8_t>(c));
  }
  return n;
}

class WebServer::Impl {
 public:
  struct Connection {
    SimSocket socket = kInvalidSocket;
    std::string buffer;
    unsigned long acceptedAt = 0;
  };

  SimSocket listener = kInvalidSocket;
  std::vector<Connection> connections;
  bool running = false;
};

WebServer::WebServer(uint16_t port) : port_(port), impl_(std::make_unique<Impl>()) {}

WebServer::~WebServer() { stop(); }

void WebServer::on(const char* path, HTTPMethod method, std::function<void()> fn) {
  routes_.push_back(Route{String(path ? path : ""), method, std::move(fn), {}});
}

void WebServer::on(const char* path, HTTPMethod method, std::function<void()> fn, std::function<void()> uploadFn) {
  routes_.push_back(Route{String(path ? path : ""), method, std::move(fn), std::move(uploadFn)});
}

void WebServer::onNotFound(std::function<void()> fn) { notFoundHandler_ = std::move(fn); }

void WebServer::begin() {
  if (!impl_ || impl_->running) {
    return;
  }
  if (!ensureSocketRuntime()) {
    return;
  }

  impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (impl_->listener == kInvalidSocket) {
    return;
  }

  int reuse = 1;
#ifdef _WIN32
  setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
  setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(impl_->listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    closeSocket(impl_->listener);
    impl_->listener = kInvalidSocket;
    return;
  }

  if (listen(impl_->listener, 8) != 0) {
    closeSocket(impl_->listener);
    impl_->listener = kInvalidSocket;
    return;
  }

  if (!setSocketNonBlocking(impl_->listener)) {
    closeSocket(impl_->listener);
    impl_->listener = kInvalidSocket;
    return;
  }

  impl_->running = true;
}

void WebServer::stop() {
  if (!impl_) {
    return;
  }

  for (auto& connection : impl_->connections) {
    closeSocket(connection.socket);
  }
  impl_->connections.clear();

  closeSocket(impl_->listener);
  impl_->listener = kInvalidSocket;
  impl_->running = false;
}

void WebServer::handleClient() {
  if (!impl_ || !impl_->running || impl_->listener == kInvalidSocket) {
    return;
  }

  for (;;) {
    sockaddr_in clientAddr{};
#ifdef _WIN32
    int clientLen = sizeof(clientAddr);
#else
    socklen_t clientLen = sizeof(clientAddr);
#endif
    const SimSocket clientSocket =
        accept(impl_->listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (clientSocket == kInvalidSocket) {
      if (isWouldBlockError() || isInterruptedError()) {
        break;
      }
      break;
    }
    if (!setSocketNonBlocking(clientSocket)) {
      closeSocket(clientSocket);
      continue;
    }
    impl_->connections.push_back(Impl::Connection{clientSocket, {}, millis()});
  }

  auto it = impl_->connections.begin();
  while (it != impl_->connections.end()) {
    bool shouldClose = false;
    char buffer[4096];

    for (;;) {
#ifdef _WIN32
      const int received = recv(it->socket, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
      const ssize_t received = recv(it->socket, buffer, sizeof(buffer), 0);
#endif
      if (received > 0) {
        it->buffer.append(buffer, static_cast<size_t>(received));
        continue;
      }
      if (received == 0) {
        shouldClose = true;
      } else if (!isWouldBlockError() && !isInterruptedError()) {
        shouldClose = true;
      }
      break;
    }

    ParsedRequest parsed;
    if (!shouldClose && tryParseRequestBuffer(it->buffer, parsed)) {
      auto clientState = std::make_shared<WiFiClientState>();
      clientState->socket = it->socket;
      clientState->open = true;
      dispatchRequest(it->buffer, clientState);
      shouldClose = true;
    }

    if (!shouldClose && millis() - it->acceptedAt > 15000) {
      shouldClose = true;
    }

    if (shouldClose) {
      closeSocket(it->socket);
      it = impl_->connections.erase(it);
      continue;
    }

    ++it;
  }
}

void WebServer::addHandler(RequestHandler* handler) { handlers_.emplace_back(handler); }

void WebServer::collectHeaders(const char*[], size_t) {}

void WebServer::clearRequestContext() { currentRequest_ = RequestContext{}; }

void WebServer::clearResponseContext() { currentResponse_ = ResponseContext{}; }

void WebServer::sendHeadersIfNeeded(const int code, const char* type, const size_t bodyLength, const bool chunked) {
  if (currentResponse_.headersSent || !currentResponse_.clientState || !currentResponse_.clientState->open) {
    return;
  }

  std::string headerBlock = "HTTP/1.1 ";
  headerBlock += std::to_string(code);
  headerBlock += " ";
  headerBlock += reasonPhrase(code);
  headerBlock += "\r\n";

  bool hasContentType = false;
  bool hasContentLength = false;
  bool hasTransferEncoding = false;
  bool hasConnection = false;

  for (const auto& [name, value] : currentResponse_.headers) {
    const std::string lowerName = toLowerCopy(name.str());
    if (lowerName == "content-type") hasContentType = true;
    if (lowerName == "content-length") hasContentLength = true;
    if (lowerName == "transfer-encoding") hasTransferEncoding = true;
    if (lowerName == "connection") hasConnection = true;

    headerBlock += name.str();
    headerBlock += ": ";
    headerBlock += value.str();
    headerBlock += "\r\n";
  }

  if (type && *type && !hasContentType) {
    headerBlock += "Content-Type: ";
    headerBlock += type;
    headerBlock += "\r\n";
  }
  if (chunked) {
    if (!hasTransferEncoding) {
      headerBlock += "Transfer-Encoding: chunked\r\n";
    }
  } else if (!hasContentLength) {
    headerBlock += "Content-Length: ";
    headerBlock += std::to_string(bodyLength);
    headerBlock += "\r\n";
  }
  if (!hasConnection) {
    headerBlock += "Connection: close\r\n";
  }
  headerBlock += "\r\n";

  writeResponseBody(reinterpret_cast<const uint8_t*>(headerBlock.data()), headerBlock.size());
  currentResponse_.headersSent = true;
  currentResponse_.chunked = chunked;
}

void WebServer::writeResponseBody(const uint8_t* data, const size_t size) {
  if (!data || size == 0 || !currentResponse_.clientState || !currentResponse_.clientState->open) {
    return;
  }
  if (!socketSendAll(currentResponse_.clientState->socket, data, size)) {
    currentResponse_.clientState->open = false;
    closeSocket(currentResponse_.clientState->socket);
    currentResponse_.clientState->socket = kInvalidSocket;
  }
}

void WebServer::maybeFinalizeChunkedResponse() {
  if (currentResponse_.chunked && currentResponse_.headersSent && !currentResponse_.chunkedEnded) {
    static constexpr char kChunkEnd[] = "0\r\n\r\n";
    writeResponseBody(reinterpret_cast<const uint8_t*>(kChunkEnd), sizeof(kChunkEnd) - 1);
    currentResponse_.chunkedEnded = true;
  }
}

void WebServer::send(int code, const char* type, const char* content) {
  const char* safeContent = content ? content : "";
  const size_t contentSize = std::strlen(safeContent);
  const bool useChunked = currentResponse_.contentLengthMode == CONTENT_LENGTH_UNKNOWN;
  const size_t bodyLength =
      currentResponse_.contentLengthMode >= 0 ? static_cast<size_t>(currentResponse_.contentLengthMode) : contentSize;
  sendHeadersIfNeeded(code, type, useChunked ? 0 : bodyLength, useChunked);
  if (contentSize > 0) {
    sendContent(safeContent);
  }
}

void WebServer::send(int code, const char* type, const String& content) {
  const bool useChunked = currentResponse_.contentLengthMode == CONTENT_LENGTH_UNKNOWN;
  const size_t bodyLength = currentResponse_.contentLengthMode >= 0 ? static_cast<size_t>(currentResponse_.contentLengthMode)
                                                                    : content.length();
  sendHeadersIfNeeded(code, type, useChunked ? 0 : bodyLength, useChunked);
  if (!content.isEmpty()) {
    sendContent(content);
  }
}

void WebServer::send(int code) { sendHeadersIfNeeded(code, "text/plain", 0, false); }

void WebServer::sendContent(const char* content) {
  const char* safeContent = content ? content : "";
  const size_t contentSize = std::strlen(safeContent);
  if (!currentResponse_.headersSent) {
    const bool useChunked = currentResponse_.contentLengthMode == CONTENT_LENGTH_UNKNOWN;
    sendHeadersIfNeeded(200, "text/plain", useChunked ? 0 : contentSize, useChunked);
  }

  if (currentResponse_.chunked) {
    if (contentSize == 0) {
      maybeFinalizeChunkedResponse();
      return;
    }
    std::ostringstream prefix;
    prefix << std::hex << contentSize << "\r\n";
    const std::string prefixString = prefix.str();
    writeResponseBody(reinterpret_cast<const uint8_t*>(prefixString.data()), prefixString.size());
    writeResponseBody(reinterpret_cast<const uint8_t*>(safeContent), contentSize);
    static constexpr char kChunkSuffix[] = "\r\n";
    writeResponseBody(reinterpret_cast<const uint8_t*>(kChunkSuffix), sizeof(kChunkSuffix) - 1);
    return;
  }

  writeResponseBody(reinterpret_cast<const uint8_t*>(safeContent), contentSize);
}

void WebServer::sendContent(const String& content) {
  if (!currentResponse_.headersSent) {
    const bool useChunked = currentResponse_.contentLengthMode == CONTENT_LENGTH_UNKNOWN;
    sendHeadersIfNeeded(200, "text/plain", useChunked ? 0 : content.length(), useChunked);
  }
  if (currentResponse_.chunked) {
    if (content.isEmpty()) {
      maybeFinalizeChunkedResponse();
      return;
    }
    std::ostringstream prefix;
    prefix << std::hex << content.length() << "\r\n";
    const std::string prefixString = prefix.str();
    writeResponseBody(reinterpret_cast<const uint8_t*>(prefixString.data()), prefixString.size());
    writeResponseBody(reinterpret_cast<const uint8_t*>(content.str().data()), content.length());
    static constexpr char kChunkSuffix[] = "\r\n";
    writeResponseBody(reinterpret_cast<const uint8_t*>(kChunkSuffix), sizeof(kChunkSuffix) - 1);
    return;
  }
  writeResponseBody(reinterpret_cast<const uint8_t*>(content.str().data()), content.length());
}

void WebServer::send_P(int code, const char* type, const char* content, size_t len) {
  const bool useChunked = currentResponse_.contentLengthMode == CONTENT_LENGTH_UNKNOWN;
  const size_t bodyLength =
      currentResponse_.contentLengthMode >= 0 ? static_cast<size_t>(currentResponse_.contentLengthMode) : len;
  sendHeadersIfNeeded(code, type, useChunked ? 0 : bodyLength, useChunked);
  if (len > 0) {
    if (currentResponse_.chunked) {
      std::ostringstream prefix;
      prefix << std::hex << len << "\r\n";
      const std::string prefixString = prefix.str();
      writeResponseBody(reinterpret_cast<const uint8_t*>(prefixString.data()), prefixString.size());
      writeResponseBody(reinterpret_cast<const uint8_t*>(content), len);
      static constexpr char kChunkSuffix[] = "\r\n";
      writeResponseBody(reinterpret_cast<const uint8_t*>(kChunkSuffix), sizeof(kChunkSuffix) - 1);
    } else {
      writeResponseBody(reinterpret_cast<const uint8_t*>(content), len);
    }
  }
}

void WebServer::sendHeader(const char* name, const char* value, const bool first) {
  sendHeader(String(name ? name : ""), String(value ? value : ""), first);
}

void WebServer::sendHeader(const String& name, const String& value, const bool first) {
  if (first) {
    currentResponse_.headers.insert(currentResponse_.headers.begin(), std::make_pair(name, value));
  } else {
    currentResponse_.headers.emplace_back(name, value);
  }
}

void WebServer::setContentLength(const size_t len) {
  if (len == static_cast<size_t>(CONTENT_LENGTH_UNKNOWN) || len == (std::numeric_limits<size_t>::max)()) {
    currentResponse_.contentLengthMode = CONTENT_LENGTH_UNKNOWN;
  } else {
    currentResponse_.contentLengthMode = static_cast<long long>(len);
  }
}

WiFiClient WebServer::client() { return WiFiClient(currentResponse_.clientState); }

bool WebServer::hasArg(const char* name) const {
  if (!name) {
    return false;
  }
  return currentRequest_.args.find(name) != currentRequest_.args.end();
}

String WebServer::arg(const char* name) const {
  if (!name) {
    return String("");
  }
  const auto it = currentRequest_.args.find(name);
  if (it == currentRequest_.args.end()) {
    return String("");
  }
  return it->second;
}

String WebServer::uri() const { return currentRequest_.uri; }

String WebServer::header(const char* name) const {
  if (!name) {
    return String("");
  }
  const auto it = currentRequest_.headers.find(toLowerCopy(name));
  if (it == currentRequest_.headers.end()) {
    return String("");
  }
  return it->second;
}

HTTPMethod WebServer::method() const { return currentRequest_.method; }

size_t WebServer::clientContentLength() const { return currentRequest_.contentLength; }

const HTTPUpload& WebServer::upload() const { return upload_; }

String WebServer::urlDecode(const String& value) { return String(decodeUrl(value.str())); }

bool WebServer::runMultipartUpload(const Route& route) {
  if (!route.uploadHandler || !headerContains(currentRequest_.headers, "content-type", "multipart/form-data")) {
    return false;
  }

  const auto contentTypeIt = currentRequest_.headers.find("content-type");
  if (contentTypeIt == currentRequest_.headers.end()) {
    return false;
  }

  const std::string boundary = extractHeaderParam(contentTypeIt->second.str(), "boundary");
  if (boundary.empty()) {
    return false;
  }

  const std::string delimiter = "--" + boundary;
  const std::string boundaryMarker = "\r\n" + delimiter;
  const std::string& body = currentRequest_.body;
  size_t cursor = 0;
  bool uploadedFile = false;

  while (true) {
    size_t partStart = body.find(delimiter, cursor);
    if (partStart == std::string::npos) {
      break;
    }
    partStart += delimiter.size();
    if (partStart + 2 <= body.size() && body.compare(partStart, 2, "--") == 0) {
      break;
    }
    if (partStart + 2 <= body.size() && body.compare(partStart, 2, "\r\n") == 0) {
      partStart += 2;
    }

    const size_t partHeaderEnd = body.find("\r\n\r\n", partStart);
    if (partHeaderEnd == std::string::npos) {
      break;
    }

    std::map<std::string, std::string> partHeaders;
    std::istringstream partHeaderStream(body.substr(partStart, partHeaderEnd - partStart));
    std::string line;
    while (std::getline(partHeaderStream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      const size_t colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      partHeaders[toLowerCopy(trimCopy(line.substr(0, colon)))] = trimCopy(line.substr(colon + 1));
    }

    const auto dispositionIt = partHeaders.find("content-disposition");
    if (dispositionIt == partHeaders.end()) {
      cursor = partHeaderEnd + 4;
      continue;
    }

    const std::string filename = extractHeaderParam(dispositionIt->second, "filename");
    const std::string fieldName = extractHeaderParam(dispositionIt->second, "name");
    const size_t dataStart = partHeaderEnd + 4;
    const size_t nextBoundary = body.find(boundaryMarker, dataStart);
    if (nextBoundary == std::string::npos) {
      break;
    }
    const size_t dataEnd = nextBoundary;

    if (!filename.empty()) {
      uploadedFile = true;
      upload_ = HTTPUpload{};
      upload_.status = UPLOAD_FILE_START;
      upload_.filename = String(filename);
      upload_.name = String(fieldName);
      const auto typeIt = partHeaders.find("content-type");
      if (typeIt != partHeaders.end()) {
        upload_.type = String(typeIt->second);
      }
      upload_.totalSize = dataEnd - dataStart;
      route.uploadHandler();

      constexpr size_t kChunkSize = 4096;
      for (size_t offset = dataStart; offset < dataEnd; offset += kChunkSize) {
        const size_t chunkSize = (std::min)(kChunkSize, dataEnd - offset);
        upload_.status = UPLOAD_FILE_WRITE;
        upload_.buf = reinterpret_cast<const uint8_t*>(body.data() + offset);
        upload_.currentSize = chunkSize;
        upload_.size += chunkSize;
        route.uploadHandler();
      }

      upload_.status = UPLOAD_FILE_END;
      upload_.buf = nullptr;
      upload_.currentSize = 0;
      route.uploadHandler();
      break;
    }

    cursor = nextBoundary + 2;
  }

  if (!uploadedFile) {
    upload_ = HTTPUpload{};
    upload_.status = UPLOAD_FILE_ABORTED;
    route.uploadHandler();
  }

  return uploadedFile;
}

bool WebServer::dispatchRoute(const Route& route) {
  if (route.uploadHandler && headerContains(currentRequest_.headers, "content-type", "multipart/form-data")) {
    runMultipartUpload(route);
  }
  if (route.handler) {
    route.handler();
  }
  return true;
}

bool WebServer::dispatchCustomHandler() {
  for (const auto& handler : handlers_) {
    if (handler && handler->canHandle(*this, currentRequest_.method, currentRequest_.uri)) {
      return handler->handle(*this, currentRequest_.method, currentRequest_.uri);
    }
  }
  return false;
}

bool WebServer::dispatchRequest(const std::string& rawRequest, const std::shared_ptr<WiFiClientState>& clientState) {
  ParsedRequest parsed;
  if (!tryParseRequestBuffer(rawRequest, parsed)) {
    return false;
  }

  clearRequestContext();
  clearResponseContext();

  currentRequest_.method = parsed.method;
  currentRequest_.headers = parsed.headers;
  currentRequest_.body = parsed.body;
  currentRequest_.contentLength = parsed.contentLength;

  const size_t queryPos = parsed.target.find('?');
  const std::string path = queryPos == std::string::npos ? parsed.target : parsed.target.substr(0, queryPos);
  currentRequest_.uri = String(path.empty() ? "/" : path);
  if (queryPos != std::string::npos && queryPos + 1 < parsed.target.size()) {
    parseUrlEncodedArgs(parsed.target.substr(queryPos + 1), currentRequest_.args);
  }

  if (!currentRequest_.body.empty()) {
    const bool isMultipart = headerContains(currentRequest_.headers, "content-type", "multipart/form-data");
    if (!isMultipart) {
      currentRequest_.args["plain"] = String(currentRequest_.body);
    }
    if (headerContains(currentRequest_.headers, "content-type", "application/x-www-form-urlencoded")) {
      parseUrlEncodedArgs(currentRequest_.body, currentRequest_.args);
    }
  }

  if (!clientState) {
    return false;
  }

  currentResponse_.clientState = clientState;

  bool handled = false;
  for (const auto& route : routes_) {
    if (route.path == currentRequest_.uri && (route.method == HTTP_ANY || route.method == currentRequest_.method)) {
      handled = dispatchRoute(route);
      break;
    }
  }

  if (!handled) {
    handled = dispatchCustomHandler();
  }

  if (!handled) {
    if (notFoundHandler_) {
      notFoundHandler_();
    } else {
      send(404, "text/plain", "Not found");
    }
  }

  maybeFinalizeChunkedResponse();
  return true;
}
