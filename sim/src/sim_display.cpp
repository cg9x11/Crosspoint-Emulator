#include "EInkDisplay.h"
#include "HalDisplay.h"
#include "HalGPIO.h"
#include "sim_spi_bus.h"

#include <SDL.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

// Rotated 90 degrees clockwise: logical 800x480 -> window 480x800
static const int WINDOW_WIDTH = static_cast<int>(EInkDisplay::DISPLAY_HEIGHT);   // 480
static const int WINDOW_HEIGHT = static_cast<int>(EInkDisplay::DISPLAY_WIDTH);   // 800
static const int TOOLBAR_WIDTH = 52;
static const int WINDOW_TOTAL_WIDTH = WINDOW_WIDTH + TOOLBAR_WIDTH;

static SDL_Window* g_window = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture* g_texture = nullptr;

namespace {
bool g_hasBw = false;
bool g_hasGrayLsb = false;
bool g_hasGrayMsb = false;
uint8_t g_bwBuffer[EInkDisplay::BUFFER_SIZE];
uint8_t g_grayLsbBuffer[EInkDisplay::BUFFER_SIZE];
uint8_t g_grayMsbBuffer[EInkDisplay::BUFFER_SIZE];

struct ToolbarButton {
  uint8_t button;
  SDL_Rect rect;
};

constexpr int TOOLBAR_PADDING = 8;
constexpr int TOOLBAR_BUTTON_GAP = 6;
constexpr int TOOLBAR_BUTTON_COUNT = 7;
constexpr int TOOLBAR_BUTTON_HEIGHT = 32;
uint8_t g_toolbarState = 0;
struct ToolbarIcon {
  uint8_t button;
  const char* fileName;
  SDL_Texture* texture = nullptr;
};

ToolbarButton makeToolbarButton(const int index, const uint8_t button) {
  const int buttonWidth = TOOLBAR_WIDTH - (TOOLBAR_PADDING * 2);
  int top = TOOLBAR_PADDING;
  switch (button) {
    case HalGPIO::BTN_LEFT:
      top = TOOLBAR_PADDING;
      break;
    case HalGPIO::BTN_RIGHT:
      top = TOOLBAR_PADDING + 1 * (TOOLBAR_BUTTON_HEIGHT + TOOLBAR_BUTTON_GAP);
      break;
    case HalGPIO::BTN_UP:
      top = TOOLBAR_PADDING + 2 * (TOOLBAR_BUTTON_HEIGHT + TOOLBAR_BUTTON_GAP);
      break;
    case HalGPIO::BTN_DOWN:
      top = TOOLBAR_PADDING + 3 * (TOOLBAR_BUTTON_HEIGHT + TOOLBAR_BUTTON_GAP);
      break;
    case HalGPIO::BTN_BACK:
      top = WINDOW_HEIGHT - TOOLBAR_PADDING - (TOOLBAR_BUTTON_HEIGHT * 3) - (TOOLBAR_BUTTON_GAP * 2);
      break;
    case HalGPIO::BTN_CONFIRM:
      top = WINDOW_HEIGHT - TOOLBAR_PADDING - (TOOLBAR_BUTTON_HEIGHT * 2) - TOOLBAR_BUTTON_GAP;
      break;
    case HalGPIO::BTN_POWER:
      top = WINDOW_HEIGHT - TOOLBAR_PADDING - TOOLBAR_BUTTON_HEIGHT;
      break;
    default:
      break;
  }
  return ToolbarButton{
      button,
      SDL_Rect{WINDOW_WIDTH + TOOLBAR_PADDING, top, buttonWidth, TOOLBAR_BUTTON_HEIGHT},
  };
}

const ToolbarButton* toolbarButtons() {
  static const ToolbarButton buttons[] = {
      makeToolbarButton(0, HalGPIO::BTN_BACK),
      makeToolbarButton(1, HalGPIO::BTN_CONFIRM),
      makeToolbarButton(2, HalGPIO::BTN_LEFT),
      makeToolbarButton(3, HalGPIO::BTN_RIGHT),
      makeToolbarButton(4, HalGPIO::BTN_UP),
      makeToolbarButton(5, HalGPIO::BTN_DOWN),
      makeToolbarButton(6, HalGPIO::BTN_POWER),
  };
  return buttons;
}

ToolbarIcon* toolbarIcons() {
  static ToolbarIcon icons[] = {
      {HalGPIO::BTN_BACK, "assets/toolbar/back.bmp"},
      {HalGPIO::BTN_CONFIRM, "assets/toolbar/confirm.bmp"},
      {HalGPIO::BTN_LEFT, "assets/toolbar/left.bmp"},
      {HalGPIO::BTN_RIGHT, "assets/toolbar/right.bmp"},
      {HalGPIO::BTN_UP, "assets/toolbar/up.bmp"},
      {HalGPIO::BTN_DOWN, "assets/toolbar/down.bmp"},
      {HalGPIO::BTN_POWER, "assets/toolbar/power.bmp"},
  };
  return icons;
}

std::string resolveToolbarIconPath(const char* relativePath) {
  if (!relativePath || !*relativePath) {
    return {};
  }

  namespace fs = std::filesystem;
  std::error_code ec;

  if (char* basePath = SDL_GetBasePath()) {
    fs::path candidate = fs::path(basePath) / relativePath;
    SDL_free(basePath);
    if (fs::exists(candidate, ec)) {
      return candidate.string();
    }
  }

  const fs::path cwdCandidate = fs::current_path(ec) / relativePath;
  if (!ec && fs::exists(cwdCandidate, ec)) {
    return cwdCandidate.string();
  }

  const fs::path repoCandidate = fs::current_path(ec) / "sim" / relativePath;
  if (!ec && fs::exists(repoCandidate, ec)) {
    return repoCandidate.string();
  }

  return relativePath;
}

SDL_Texture* iconTextureForButton(const uint8_t button) {
  ToolbarIcon* icons = toolbarIcons();
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
    if (icons[i].button == button) {
      return icons[i].texture;
    }
  }
  return nullptr;
}

void loadToolbarIcons() {
  if (!g_renderer) return;
  ToolbarIcon* icons = toolbarIcons();
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
    if (icons[i].texture) {
      continue;
    }
    const std::string iconPath = resolveToolbarIconPath(icons[i].fileName);
    SDL_Surface* surface = SDL_LoadBMP(iconPath.c_str());
    if (!surface) {
      SDL_Log("Toolbar icon load failed: %s (%s)", iconPath.c_str(), SDL_GetError());
      continue;
    }
    SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGB(surface->format, 255, 255, 255));
    icons[i].texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
  }
}

void destroyToolbarIcons() {
  ToolbarIcon* icons = toolbarIcons();
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
    if (icons[i].texture) {
      SDL_DestroyTexture(icons[i].texture);
      icons[i].texture = nullptr;
    }
  }
}

void drawButtonOutline(const SDL_Rect& rect, const bool pressed) {
  const Uint8 fill = pressed ? 32 : 244;
  const Uint8 border = pressed ? 255 : 60;
  SDL_SetRenderDrawColor(g_renderer, fill, fill, fill, 255);
  SDL_RenderFillRect(g_renderer, &rect);
  SDL_SetRenderDrawColor(g_renderer, border, border, border, 255);
  SDL_RenderDrawRect(g_renderer, &rect);
  if (rect.w > 8 && rect.h > 8) {
    SDL_Rect inner{rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4};
    SDL_RenderDrawRect(g_renderer, &inner);
  }
}

void drawBitmapIcon(const SDL_Rect& rect, const uint8_t button, const bool pressed) {
  SDL_Texture* texture = iconTextureForButton(button);
  if (!texture) return;

  SDL_SetTextureColorMod(texture, pressed ? 245 : 20, pressed ? 245 : 20, pressed ? 245 : 20);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

  const SDL_Rect dest{
      rect.x + (rect.w - 20) / 2,
      rect.y + (rect.h - 20) / 2,
      20,
      20,
  };
  SDL_RenderCopy(g_renderer, texture, nullptr, &dest);
}

void drawFallbackIcon(const SDL_Rect& rect, const bool pressed) {
  const Uint8 stroke = pressed ? 245 : 20;
  SDL_SetRenderDrawColor(g_renderer, stroke, stroke, stroke, 255);
  const SDL_Rect box{rect.x + rect.w / 3, rect.y + rect.h / 3, rect.w / 3, rect.h / 3};
  SDL_RenderDrawRect(g_renderer, &box);
}

void drawToolbarIcon(const SDL_Rect& rect, const uint8_t button, const bool pressed) {
  if (iconTextureForButton(button)) {
    drawBitmapIcon(rect, button, pressed);
  } else {
    drawFallbackIcon(rect, pressed);
  }
}

void drawToolbarButton(const ToolbarButton& button, const bool pressed) {
  drawButtonOutline(button.rect, pressed);
  drawToolbarIcon(button.rect, button.button, pressed);
}

void renderToWindow() {
  if (!g_renderer || !g_texture) return;

  SDL_SetRenderDrawColor(g_renderer, 250, 250, 250, 255);
  SDL_RenderClear(g_renderer);

  const SDL_Rect screenRect{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
  SDL_RenderCopy(g_renderer, g_texture, nullptr, &screenRect);

  const SDL_Rect toolbarRect{WINDOW_WIDTH, 0, TOOLBAR_WIDTH, WINDOW_HEIGHT};
  SDL_SetRenderDrawColor(g_renderer, 224, 224, 224, 255);
  SDL_RenderFillRect(g_renderer, &toolbarRect);
  SDL_SetRenderDrawColor(g_renderer, 110, 110, 110, 255);
  SDL_RenderDrawLine(g_renderer, WINDOW_WIDTH, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

  const ToolbarButton* buttons = toolbarButtons();
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
    const bool pressed = (g_toolbarState & (1 << buttons[i].button)) != 0;
    drawToolbarButton(buttons[i], pressed);
  }

  SDL_RenderPresent(g_renderer);
}

void renderBwToTexture(const uint8_t* buf) {
  if (!g_renderer || !g_texture || !buf) return;
  uint8_t* pixels = nullptr;
  int pitch = 0;
  if (SDL_LockTexture(g_texture, nullptr, reinterpret_cast<void**>(&pixels), &pitch) != 0) return;

  const int height = static_cast<int>(EInkDisplay::DISPLAY_HEIGHT);
  const int width = static_cast<int>(EInkDisplay::DISPLAY_WIDTH);
  const int widthBytes = static_cast<int>(EInkDisplay::DISPLAY_WIDTH_BYTES);

  for (int y = 0; y < height; y++) {
    const int rotX = height - 1 - y;
    const size_t rowBase = static_cast<size_t>(y) * widthBytes;

    for (int byteIdx = 0; byteIdx < widthBytes; byteIdx++) {
      const uint8_t byte = buf[rowBase + byteIdx];
      const int xBase = byteIdx * 8;
      const int remaining = width - xBase;
      const int count = remaining < 8 ? remaining : 8;

      for (int b = 0; b < count; b++) {
        const uint8_t v = (byte & (0x80 >> b)) ? 255 : 0;
        const int rotY = xBase + b;
        const size_t off = static_cast<size_t>(rotY) * static_cast<size_t>(pitch) +
                           static_cast<size_t>(rotX) * 3;
        pixels[off + 0] = v;
        pixels[off + 1] = v;
        pixels[off + 2] = v;
      }
    }
  }

  SDL_UnlockTexture(g_texture);
  renderToWindow();
}

void renderGrayToTexture(const uint8_t* bw, const uint8_t* lsb, const uint8_t* msb) {
  if (!g_renderer || !g_texture || !bw || !lsb || !msb) return;
  uint8_t* pixels = nullptr;
  int pitch = 0;
  if (SDL_LockTexture(g_texture, nullptr, reinterpret_cast<void**>(&pixels), &pitch) != 0) return;

  static constexpr uint8_t grayLut[8] = {
      0,
      0,
      170,
      85,
      255,
      255,
      255,
      255,
  };

  const int height = static_cast<int>(EInkDisplay::DISPLAY_HEIGHT);
  const int width = static_cast<int>(EInkDisplay::DISPLAY_WIDTH);
  const int widthBytes = static_cast<int>(EInkDisplay::DISPLAY_WIDTH_BYTES);

  for (int y = 0; y < height; y++) {
    const int rotX = height - 1 - y;
    const size_t rowBase = static_cast<size_t>(y) * widthBytes;

    for (int byteIdx = 0; byteIdx < widthBytes; byteIdx++) {
      const uint8_t bwByte = bw[rowBase + byteIdx];
      const uint8_t lsbByte = lsb[rowBase + byteIdx];
      const uint8_t msbByte = msb[rowBase + byteIdx];
      const int xBase = byteIdx * 8;
      const int remaining = width - xBase;
      const int count = remaining < 8 ? remaining : 8;

      for (int b = 0; b < count; b++) {
        const uint8_t mask = 0x80 >> b;
        const int lutIdx = ((bwByte & mask) ? 4 : 0) | ((msbByte & mask) ? 2 : 0) |
                           ((lsbByte & mask) ? 1 : 0);
        const uint8_t v = grayLut[lutIdx];
        const int rotY = xBase + b;
        const size_t off = static_cast<size_t>(rotY) * static_cast<size_t>(pitch) +
                           static_cast<size_t>(rotX) * 3;
        pixels[off + 0] = v;
        pixels[off + 1] = v;
        pixels[off + 2] = v;
      }
    }
  }

  SDL_UnlockTexture(g_texture);
  renderToWindow();
}
}  // namespace

EInkDisplay::EInkDisplay(int8_t, int8_t, int8_t, int8_t, int8_t, int8_t)
    : frameBuffer(frameBuffer0), isScreenOn(false) {}

bool sim_display_init(void) {
  if (g_window) return true;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
  g_window = SDL_CreateWindow("Crosspoint Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WINDOW_TOTAL_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
  if (!g_window) return false;
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  if (!g_renderer) return false;
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                WINDOW_WIDTH, WINDOW_HEIGHT);
  if (!g_texture) return false;
  loadToolbarIcons();
  renderToWindow();
  return true;
}

bool sim_display_save_screenshot(const char* path) {
  if (!g_renderer || !g_window || !path || !*path) return false;

  int width = 0;
  int height = 0;
  SDL_GetRendererOutputSize(g_renderer, &width, &height);
  if (width <= 0 || height <= 0) return false;

  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) return false;

  const int readResult =
      SDL_RenderReadPixels(g_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch);
  bool ok = false;
  if (readResult == 0) {
    ok = SDL_SaveBMP(surface, path) == 0;
  }
  SDL_FreeSurface(surface);
  return ok;
}

void sim_display_shutdown(void) {
  destroyToolbarIcons();
  if (g_texture) {
    SDL_DestroyTexture(g_texture);
    g_texture = nullptr;
  }
  if (g_renderer) {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
  }
  if (g_window) {
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
  SDL_Quit();
}

void EInkDisplay::begin() {
  if (g_window) {
    frameBuffer = frameBuffer0;
    memset(frameBuffer0, 0xFF, EInkDisplay::BUFFER_SIZE);
    isScreenOn = true;
    renderToWindow();
    return;
  }
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return;
  g_window = SDL_CreateWindow("Crosspoint Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WINDOW_TOTAL_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
  if (!g_window) return;
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  if (!g_renderer) return;
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                WINDOW_WIDTH, WINDOW_HEIGHT);
  if (!g_texture) return;
  loadToolbarIcons();
  frameBuffer = frameBuffer0;
  memset(frameBuffer0, 0xFF, EInkDisplay::BUFFER_SIZE);
  isScreenOn = true;
  renderToWindow();
}

void EInkDisplay::clearScreen(uint8_t color) const {
  memset(frameBuffer, color, EInkDisplay::BUFFER_SIZE);
}

void EInkDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool) const {
  SpiBusGuard guard;
  if (!imageData || !frameBuffer) return;
  const uint16_t rowBytes = (w + 7) / 8;
  for (uint16_t row = 0; row < h; row++) {
    uint16_t dstY = y + row;
    if (dstY >= EInkDisplay::DISPLAY_HEIGHT) break;
    for (uint16_t col = 0; col < w; col += 8) {
      uint8_t byte = imageData[row * rowBytes + col / 8];
      for (int b = 0; b < 8 && col + b < w; b++) {
        uint16_t dstX = x + col + b;
        if (dstX >= EInkDisplay::DISPLAY_WIDTH) break;
        if (byte & (0x80 >> b)) {
          size_t idx = dstY * EInkDisplay::DISPLAY_WIDTH_BYTES + dstX / 8;
          frameBuffer[idx] &= ~(0x80 >> (dstX & 7));
        }
      }
    }
  }
}

void EInkDisplay::setFramebuffer(const uint8_t*) const {}

void EInkDisplay::copyGrayscaleBuffers(const uint8_t* lsb, const uint8_t* msb) {
  if (!lsb || !msb) return;
  memcpy(g_grayLsbBuffer, lsb, EInkDisplay::BUFFER_SIZE);
  memcpy(g_grayMsbBuffer, msb, EInkDisplay::BUFFER_SIZE);
  g_hasGrayLsb = true;
  g_hasGrayMsb = true;
}
void EInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsb) {
  if (!lsb) return;
  memcpy(g_grayLsbBuffer, lsb, EInkDisplay::BUFFER_SIZE);
  g_hasGrayLsb = true;
}
void EInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msb) {
  if (!msb) return;
  memcpy(g_grayMsbBuffer, msb, EInkDisplay::BUFFER_SIZE);
  g_hasGrayMsb = true;
}
void EInkDisplay::cleanupGrayscaleBuffers(const uint8_t*) {
  g_hasGrayLsb = false;
  g_hasGrayMsb = false;
}

void EInkDisplay::displayBuffer(RefreshMode, bool) {
  SpiBusGuard guard;
  if (!frameBuffer) return;
  memcpy(g_bwBuffer, frameBuffer, EInkDisplay::BUFFER_SIZE);
  g_hasBw = true;
  g_hasGrayLsb = false;
  g_hasGrayMsb = false;
  renderBwToTexture(frameBuffer);
}

void EInkDisplay::displayWindow(uint16_t, uint16_t, uint16_t, uint16_t) {
  displayBuffer(FAST_REFRESH);
}

void EInkDisplay::displayGrayBuffer(bool) {
  SpiBusGuard guard;
  if (!g_hasBw || !g_hasGrayLsb || !g_hasGrayMsb) return;
  renderGrayToTexture(g_bwBuffer, g_grayLsbBuffer, g_grayMsbBuffer);
}
void EInkDisplay::refreshDisplay(RefreshMode mode, bool) { displayBuffer(mode); }
void EInkDisplay::grayscaleRevert() {}
void EInkDisplay::setCustomLUT(bool, const unsigned char*) {}
void EInkDisplay::deepSleep() { isScreenOn = false; }
void EInkDisplay::saveFrameBufferAsPBM(const char*) {}

HalDisplay::HalDisplay() : einkDisplay(0, 0, 0, 0, 0, 0) {}
HalDisplay::~HalDisplay() = default;

void HalDisplay::begin() { einkDisplay.begin(); }
void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }
void HalDisplay::drawImage(const uint8_t* d, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool pm) const {
  einkDisplay.drawImage(d, x, y, w, h, pm);
}
void HalDisplay::drawImageTransparent(const uint8_t* d, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool pm) const {
  einkDisplay.drawImage(d, x, y, w, h, pm);
}
void HalDisplay::displayBuffer(RefreshMode mode, bool fadingFix) {
  einkDisplay.displayBuffer(static_cast<EInkDisplay::RefreshMode>(mode), fadingFix);
}
void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOff) {
  einkDisplay.refreshDisplay(static_cast<EInkDisplay::RefreshMode>(mode), turnOff);
}
void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }
uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }
void HalDisplay::copyGrayscaleBuffers(const uint8_t* l, const uint8_t* m) {
  einkDisplay.copyGrayscaleBuffers(l, m);
}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* l) {
  einkDisplay.copyGrayscaleLsbBuffers(l);
}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* m) {
  einkDisplay.copyGrayscaleMsbBuffers(m);
}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* b) {
  einkDisplay.cleanupGrayscaleBuffers(b);
}
void HalDisplay::displayGrayBuffer(bool fadingFix) { einkDisplay.displayGrayBuffer(fadingFix); }

HalDisplay display;

bool sim_display_pump_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
      const char* envPath = std::getenv("SIM_SCREENSHOT_PATH");
      const char* screenshotPath = (envPath && *envPath) ? envPath : "sim-screenshot.bmp";
      sim_display_save_screenshot(screenshotPath);
    }
    if (e.type == SDL_QUIT) return false;
  }
  return true;
}

bool sim_display_toolbar_hit_test(int x, int y, uint8_t& outButton) {
  const ToolbarButton* buttons = toolbarButtons();
  for (int i = 0; i < TOOLBAR_BUTTON_COUNT; ++i) {
    const SDL_Rect& rect = buttons[i].rect;
    if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) {
      outButton = buttons[i].button;
      return true;
    }
  }
  return false;
}

void sim_display_set_toolbar_state(uint8_t buttonMask) {
  const uint8_t nextState = static_cast<uint8_t>(
      buttonMask &
      ((1 << HalGPIO::BTN_BACK) | (1 << HalGPIO::BTN_CONFIRM) | (1 << HalGPIO::BTN_LEFT) |
       (1 << HalGPIO::BTN_RIGHT) | (1 << HalGPIO::BTN_UP) | (1 << HalGPIO::BTN_DOWN) |
       (1 << HalGPIO::BTN_POWER)));
  if (g_toolbarState == nextState) {
    return;
  }
  g_toolbarState = nextState;
  renderToWindow();
}
