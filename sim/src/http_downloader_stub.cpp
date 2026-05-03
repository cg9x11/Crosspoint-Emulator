#include "network/HttpDownloader.h"

#include "HalStorage.h"
#include "ESP.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace {
std::string g_lastError;
bool g_lastResponseTruncated = false;
constexpr uint32_t HTTP_MIN_HEADROOM_BYTES = 12288;
constexpr int HTTP_CONNECT_TIMEOUT_MS = 8000;
constexpr int HTTP_SEND_TIMEOUT_MS = 8000;
constexpr int HTTP_RECEIVE_TIMEOUT_MS = 15000;

void setLastError(std::string message) { g_lastError = std::move(message); }

size_t computeSafeBodyBudget(size_t requestedMaxBytes = 0) {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t budget = 0;
  if (largest > HTTP_MIN_HEADROOM_BYTES) {
    budget = largest - HTTP_MIN_HEADROOM_BYTES;
  } else if (freeHeap > HTTP_MIN_HEADROOM_BYTES) {
    budget = freeHeap - HTTP_MIN_HEADROOM_BYTES;
  }
  if (requestedMaxBytes > 0 && (budget == 0 || requestedMaxBytes < budget)) {
    budget = requestedMaxBytes;
  }
  return budget;
}

#ifdef _WIN32
std::wstring toWide(const std::string& value) {
  if (value.empty()) return std::wstring();
  const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), len);
  if (!out.empty() && out.back() == L'\0') out.pop_back();
  return out;
}

std::string base64Encode(const std::string& input) {
  static constexpr char kBase64Table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0;
  int valb = -6;
  for (unsigned char c : input) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(kBase64Table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(kBase64Table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::wstring joinRequestPath(const URL_COMPONENTS& parts) {
  std::wstring requestPath;
  if (parts.dwUrlPathLength > 0 && parts.lpszUrlPath) {
    requestPath.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  } else {
    requestPath = L"/";
  }
  if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo) {
    requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  return requestPath;
}

bool queryHeaderString(HINTERNET request, DWORD infoLevel, std::wstring& outValue) {
  outValue.clear();
  DWORD sizeBytes = 0;
  if (!WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &sizeBytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sizeBytes == 0) {
      return false;
    }
  }

  outValue.resize(sizeBytes / sizeof(wchar_t));
  if (!WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, outValue.data(), &sizeBytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    outValue.clear();
    return false;
  }
  while (!outValue.empty() && outValue.back() == L'\0') {
    outValue.pop_back();
  }
  return !outValue.empty();
}

std::wstring toLowerWide(std::wstring value) {
  for (wchar_t& ch : value) {
    ch = static_cast<wchar_t>(towlower(ch));
  }
  return value;
}

bool headerContainsToken(HINTERNET request, DWORD infoLevel, const wchar_t* token) {
  std::wstring value;
  if (!queryHeaderString(request, infoLevel, value)) {
    return false;
  }
  return toLowerWide(std::move(value)).find(token) != std::wstring::npos;
}

class ChunkedDecoder {
 public:
  using EmitChunk = std::function<bool(const uint8_t*, size_t)>;

  bool process(const uint8_t* data, size_t size, const EmitChunk& emit) {
    size_t index = 0;
    while (index < size) {
      switch (state_) {
        case State::ReadSize:
          if (!consumeSizeByte(data[index++])) {
            return false;
          }
          break;
        case State::ReadData: {
          const size_t toCopy = (std::min)(chunkBytesRemaining_, size - index);
          if (toCopy > 0 && !emit(data + index, toCopy)) {
            return false;
          }
          index += toCopy;
          chunkBytesRemaining_ -= toCopy;
          if (chunkBytesRemaining_ == 0) {
            state_ = State::ExpectChunkDataLf;
          }
          break;
        }
        case State::ExpectChunkDataLf:
          if (!consumeExpectedByte(data[index++], '\n')) {
            return false;
          }
          state_ = sawFinalChunk_ ? State::ReadTrailer : State::ReadSize;
          break;
        case State::ReadTrailer:
          if (!consumeTrailerByte(data[index++])) {
            return false;
          }
          break;
        case State::Done:
          index = size;
          break;
      }
    }
    return true;
  }

  bool finish() {
    return state_ == State::Done || (state_ == State::ReadTrailer && trailerLine_.empty());
  }

 private:
  enum class State { ReadSize, ReadData, ExpectChunkDataLf, ReadTrailer, Done };

  bool consumeExpectedByte(uint8_t ch, char expected) {
    if (ch == static_cast<uint8_t>(expected)) {
      return true;
    }
    setLastError("Invalid chunked response framing");
    return false;
  }

  bool consumeSizeByte(uint8_t ch) {
    if (expectingSizeLf_) {
      expectingSizeLf_ = false;
      if (ch != '\n') {
        setLastError("Invalid chunked size line ending");
        return false;
      }

      std::string parsedSize = sizeLine_;
      const size_t extensionPos = parsedSize.find(';');
      if (extensionPos != std::string::npos) {
        parsedSize.resize(extensionPos);
      }
      while (!parsedSize.empty() && std::isspace(static_cast<unsigned char>(parsedSize.back()))) {
        parsedSize.pop_back();
      }
      size_t firstDigit = 0;
      while (firstDigit < parsedSize.size() && std::isspace(static_cast<unsigned char>(parsedSize[firstDigit]))) {
        ++firstDigit;
      }
      parsedSize.erase(0, firstDigit);
      if (parsedSize.empty()) {
        setLastError("Empty chunk size");
        return false;
      }

      chunkBytesRemaining_ = 0;
      for (char digit : parsedSize) {
        if (!std::isxdigit(static_cast<unsigned char>(digit))) {
          setLastError("Invalid chunk size");
          return false;
        }
        chunkBytesRemaining_ = (chunkBytesRemaining_ << 4) +
                               static_cast<size_t>(std::isdigit(static_cast<unsigned char>(digit))
                                                       ? (digit - '0')
                                                       : (std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10));
      }

      sizeLine_.clear();
      if (chunkBytesRemaining_ == 0) {
        sawFinalChunk_ = true;
        state_ = State::ReadTrailer;
      } else {
        state_ = State::ReadData;
      }
      return true;
    }

    if (expectingSizeCr_) {
      expectingSizeCr_ = false;
      expectingSizeLf_ = true;
      if (ch != '\r') {
        setLastError("Invalid chunked size separator");
        return false;
      }
      return true;
    }

    if (ch == '\r') {
      expectingSizeCr_ = true;
      return true;
    }

    sizeLine_.push_back(static_cast<char>(ch));
    return true;
  }

  bool consumeTrailerByte(uint8_t ch) {
    if (expectingTrailerLf_) {
      expectingTrailerLf_ = false;
      if (ch != '\n') {
        setLastError("Invalid chunk trailer ending");
        return false;
      }
      if (trailerLine_.empty()) {
        state_ = State::Done;
      } else {
        trailerLine_.clear();
      }
      return true;
    }

    if (ch == '\r') {
      expectingTrailerLf_ = true;
      return true;
    }

    trailerLine_.push_back(static_cast<char>(ch));
    return true;
  }

  State state_ = State::ReadSize;
  std::string sizeLine_;
  std::string trailerLine_;
  size_t chunkBytesRemaining_ = 0;
  bool expectingSizeCr_ = false;
  bool expectingSizeLf_ = false;
  bool expectingTrailerLf_ = false;
  bool sawFinalChunk_ = false;
};

class ResponseBodyDecoder {
 public:
  using EmitChunk = std::function<bool(const uint8_t*, size_t)>;

  explicit ResponseBodyDecoder(bool maybeChunked) : maybeChunked_(maybeChunked) {}

  bool process(const uint8_t* data, size_t size, const EmitChunk& emit) {
    if (mode_ == Mode::Plain) {
      return emit(data, size);
    }
    if (mode_ == Mode::Chunked) {
      return chunkedDecoder_.process(data, size, emit);
    }

    probeBuffer_.append(reinterpret_cast<const char*>(data), size);
    decideModeIfPossible();
    if (mode_ == Mode::Undecided) {
      return true;
    }
    if (mode_ == Mode::Plain) {
      const bool ok = emit(reinterpret_cast<const uint8_t*>(probeBuffer_.data()), probeBuffer_.size());
      probeBuffer_.clear();
      return ok;
    }
    const bool ok = chunkedDecoder_.process(reinterpret_cast<const uint8_t*>(probeBuffer_.data()), probeBuffer_.size(), emit);
    probeBuffer_.clear();
    return ok;
  }

  bool finish(const EmitChunk& emit) {
    if (mode_ == Mode::Undecided) {
      mode_ = Mode::Plain;
    }
    if (mode_ == Mode::Plain && !probeBuffer_.empty()) {
      if (!emit(reinterpret_cast<const uint8_t*>(probeBuffer_.data()), probeBuffer_.size())) {
        return false;
      }
      probeBuffer_.clear();
      return true;
    }
    if (mode_ == Mode::Chunked) {
      return chunkedDecoder_.finish();
    }
    return true;
  }

 private:
  enum class Mode { Undecided, Plain, Chunked };

  void decideModeIfPossible() {
    if (!maybeChunked_) {
      mode_ = Mode::Plain;
      return;
    }
    if (probeBuffer_.empty()) {
      return;
    }

    const size_t crlfPos = probeBuffer_.find("\r\n");
    if (crlfPos != std::string::npos) {
      const std::string firstLine = probeBuffer_.substr(0, crlfPos);
      if (looksLikeChunkSizeLine(firstLine)) {
        mode_ = Mode::Chunked;
      } else {
        mode_ = Mode::Plain;
      }
      return;
    }

    if (probeBuffer_.size() >= 16 || !prefixCouldStillBeChunkSize(probeBuffer_)) {
      mode_ = Mode::Plain;
    }
  }

  static bool looksLikeChunkSizeLine(const std::string& line) {
    if (line.empty()) {
      return false;
    }
    const size_t extensionPos = line.find(';');
    const std::string chunkSize = line.substr(0, extensionPos);
    bool sawDigit = false;
    for (char ch : chunkSize) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        continue;
      }
      if (!std::isxdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
      sawDigit = true;
    }
    return sawDigit;
  }

  static bool prefixCouldStillBeChunkSize(const std::string& prefix) {
    for (char ch : prefix) {
      if (ch == '\r') {
        return true;
      }
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == ';') {
        continue;
      }
      if (!std::isxdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
    }
    return true;
  }

  bool maybeChunked_ = false;
  Mode mode_ = Mode::Undecided;
  std::string probeBuffer_;
  ChunkedDecoder chunkedDecoder_;
};

bool downloadWithWinHttp(const std::string& url, const std::string& username, const std::string& password,
                         const std::function<bool(const uint8_t*, size_t, size_t, size_t)>& onChunk) {
  URL_COMPONENTS parts{};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  wchar_t extraInfo[2048] = {};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.lpszExtraInfo = extraInfo;
  parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extraInfo));

  std::wstring wideUrl = toWide(url);
  if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
    setLastError("Invalid URL");
    return false;
  }

  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  HINTERNET session = WinHttpOpen(L"Crosspoint-Emulator/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    setLastError("WinHTTP session open failed");
    return false;
  }
  WinHttpSetTimeouts(session, HTTP_CONNECT_TIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS, HTTP_SEND_TIMEOUT_MS,
                     HTTP_RECEIVE_TIMEOUT_MS);

  HINTERNET connect = WinHttpConnect(session, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(),
                                     parts.nPort, 0);
  if (!connect) {
    setLastError("WinHTTP connect failed");
    WinHttpCloseHandle(session);
    return false;
  }

  const std::wstring requestPath = joinRequestPath(parts);
  HINTERNET request = WinHttpOpenRequest(connect, L"GET", requestPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    setLastError("WinHTTP open request failed");
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
  DWORD decompressionFlags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
  WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompressionFlags, sizeof(decompressionFlags));

  std::wstring extraHeaders =
      L"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
      L"Accept-Language: vi-VN,vi;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
      L"Cache-Control: no-cache\r\n"
      L"Pragma: no-cache\r\n";
  if (!username.empty() || !password.empty()) {
    extraHeaders += toWide("Authorization: Basic " + base64Encode(username + ":" + password) + "\r\n");
  }

  const bool sent = WinHttpSendRequest(request,
                                       extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str(),
                                       extraHeaders.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr);
  if (!sent) {
    setLastError("WinHTTP send/receive failed");
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  const bool isChunkedTransfer = headerContainsToken(request, WINHTTP_QUERY_TRANSFER_ENCODING, L"chunked");
  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                           &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
      statusCode < 200 || statusCode >= 300) {
    setLastError("HTTP " + std::to_string(statusCode));
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  unsigned long long contentLengthValue = 0;
  DWORD contentLengthSize = sizeof(contentLengthValue);
  size_t totalBytes = 0;
  if (WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX,
                          &contentLengthValue,
                          &contentLengthSize,
                          WINHTTP_NO_HEADER_INDEX)) {
    totalBytes = static_cast<size_t>(contentLengthValue);
  }

  std::vector<uint8_t> buffer(16 * 1024);
  size_t downloaded = 0;
  size_t delivered = 0;
  ResponseBodyDecoder bodyDecoder(isChunkedTransfer);
  const auto emitDecodedChunk = [&onChunk, &delivered](const uint8_t* chunkData, size_t chunkSize, size_t totalBytes) {
    delivered += chunkSize;
    return onChunk(chunkData, chunkSize, delivered, totalBytes);
  };
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request, &available)) {
      setLastError("WinHTTP query data failed");
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
    if (available == 0) break;

    DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), toRead, &read)) {
      setLastError("WinHTTP read failed");
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
    if (read == 0) break;
    downloaded += read;
    const bool handled = bodyDecoder.process(buffer.data(), read,
                                             [&emitDecodedChunk, totalBytes](const uint8_t* chunkData, size_t chunkSize) {
                                               return emitDecodedChunk(chunkData, chunkSize, totalBytes);
                                             });
    if (!handled) {
      if (g_lastError.empty()) {
        setLastError("Stream write failed");
      }
      WinHttpCloseHandle(request);
      WinHttpCloseHandle(connect);
      WinHttpCloseHandle(session);
      return false;
    }
  }

  if (!bodyDecoder.finish([&emitDecodedChunk, totalBytes](const uint8_t* chunkData, size_t chunkSize) {
        return emitDecodedChunk(chunkData, chunkSize, totalBytes);
      })) {
    if (g_lastError.empty()) {
      setLastError(isChunkedTransfer ? "Incomplete chunked response" : "Stream write failed");
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return false;
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return true;
}
#endif
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrlCapped(url, outContent, 0, false, username, password);
}

bool HttpDownloader::fetchUrlCapped(const std::string& url, std::string& outContent, size_t maxBytes, bool allowTruncate,
                                    const std::string& username, const std::string& password) {
  clearLastError();
#ifdef _WIN32
  outContent.clear();
  const size_t safeBudget = computeSafeBodyBudget(maxBytes);
  if (safeBudget == 0) {
    setLastError("Insufficient heap for HTTP body");
    return false;
  }
  return downloadWithWinHttp(url, username, password,
                             [&outContent, safeBudget, allowTruncate](const uint8_t* data, size_t size, size_t, size_t) {
                               const size_t remaining = safeBudget > outContent.size() ? safeBudget - outContent.size() : 0;
                               if (remaining == 0) {
                                 if (!allowTruncate) {
                                   setLastError("HTTP body exceeds safe heap budget");
                                  } else {
                                    g_lastResponseTruncated = true;
                                    setLastError("Response truncated by RAM limit");
                                  }
                                  return allowTruncate;
                                }

                                const size_t toCopy = (std::min)(remaining, size);
                                outContent.append(reinterpret_cast<const char*>(data), toCopy);
                                if (toCopy < size && !allowTruncate) {
                                  setLastError("HTTP body exceeds safe heap budget");
                                  return false;
                                }
                                if (toCopy < size && allowTruncate) {
                                  g_lastResponseTruncated = true;
                                  setLastError("Response truncated by RAM limit");
                                }
                                return true;
                              });
#else
  setLastError("HTTP unavailable in this emulator build");
  (void)url;
  (void)outContent;
  (void)username;
  (void)password;
  return false;
#endif
}

bool HttpDownloader::fetchUrlFromMarkerCapped(const std::string& url, std::string& outContent, const std::string& marker,
                                              size_t maxBytes, bool allowTruncate, const std::string& username,
                                              const std::string& password) {
  if (!fetchUrlCapped(url, outContent, maxBytes, allowTruncate, username, password)) {
    return false;
  }
  if (marker.empty()) {
    setLastError("Missing HTTP marker");
    return false;
  }
  const size_t markerPos = outContent.find(marker);
  if (markerPos == std::string::npos) {
    setLastError("Content marker not found");
    outContent.clear();
    return false;
  }
  if (markerPos > 0) {
    outContent.erase(0, markerPos);
  }
  return true;
}

bool HttpDownloader::fetchUrlFromMarkerStreamed(const std::string& url, const std::string& marker, ChunkCallback onChunk,
                                                const std::string& username, const std::string& password) {
  clearLastError();
#ifdef _WIN32
  if (marker.empty()) {
    setLastError("Missing HTTP marker");
    return false;
  }

  std::string tail;
  tail.reserve((std::max)(marker.size(), static_cast<size_t>(32)));
  bool started = false;
  bool consumerStopped = false;

  const bool ok = downloadWithWinHttp(
      url, username, password,
      [&marker, &onChunk, &tail, &started, &consumerStopped](const uint8_t* data, size_t size, size_t, size_t) {
        if (consumerStopped || size == 0) {
          return true;
        }

        if (!started) {
          std::string candidate = tail;
          candidate.append(reinterpret_cast<const char*>(data), size);
          const size_t markerPos = candidate.find(marker);
          if (markerPos == std::string::npos) {
            const size_t keep = marker.size() > 1 ? marker.size() - 1 : 0;
            if (candidate.size() > keep) {
              tail.assign(candidate.data() + candidate.size() - keep, keep);
            } else {
              tail = std::move(candidate);
            }
            return true;
          }

          started = true;
          tail.clear();
          if (!onChunk) {
            return true;
          }

          const uint8_t* chunkPtr = reinterpret_cast<const uint8_t*>(candidate.data() + markerPos);
          const size_t chunkSize = candidate.size() - markerPos;
          if (chunkSize == 0) {
            return true;
          }
          if (!onChunk(chunkPtr, chunkSize)) {
            consumerStopped = true;
          }
          return true;
        }

        if (!onChunk) {
          return true;
        }
        if (!onChunk(data, size)) {
          consumerStopped = true;
        }
        return true;
      });

  if (!ok) {
    return false;
  }
  if (!started) {
    setLastError("Content marker not found");
    return false;
  }
  return true;
#else
  setLastError("HTTP unavailable in this emulator build");
  (void)url;
  (void)marker;
  (void)onChunk;
  (void)username;
  (void)password;
  return false;
#endif
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& stream, const std::string& username,
                              const std::string& password) {
  clearLastError();
#ifdef _WIN32
  return downloadWithWinHttp(url, username, password,
                             [&stream](const uint8_t* data, size_t size, size_t, size_t) {
                               size_t written = 0;
                               while (written < size) {
                                 const size_t chunk = stream.write(data + written, size - written);
                                 if (chunk == 0) return false;
                                 written += chunk;
                               }
                               return true;
                             });
#else
  setLastError("HTTP unavailable in this emulator build");
  (void)url;
  (void)stream;
  (void)username;
  (void)password;
  return false;
#endif
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password) {
  clearLastError();
#ifdef _WIN32
  auto& storage = HalStorage::getInstance();
  if (storage.exists(destPath.c_str())) {
    storage.remove(destPath.c_str());
  }

  FsFile file = storage.open(destPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY);
  if (!file) {
    setLastError("Failed to open destination file");
    return FILE_ERROR;
  }

  const bool ok = downloadWithWinHttp(url, username, password,
                                      [&file, &progress](const uint8_t* data, size_t size, size_t downloaded,
                                                         size_t total) {
                                        if (file.write(data, size) != size) {
                                          return false;
                                        }
                                        if (progress) progress(downloaded, total);
                                        return true;
                                      });
  file.close();
  if (!ok) {
    if (g_lastError.empty()) {
      setLastError("Download failed");
    }
    storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  return OK;
#else
  setLastError("HTTP unavailable in this emulator build");
  (void)url;
  (void)destPath;
  (void)progress;
  (void)username;
  (void)password;
  return HTTP_ERROR;
#endif
}

const std::string& HttpDownloader::getLastError() { return g_lastError; }

void HttpDownloader::clearLastError() {
  g_lastError.clear();
  g_lastResponseTruncated = false;
}

bool HttpDownloader::wasLastResponseTruncated() { return g_lastResponseTruncated; }
