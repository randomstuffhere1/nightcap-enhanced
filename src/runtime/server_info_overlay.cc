#include "runtime/server_info_overlay.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <arpa/inet.h>
#include <fontconfig/fontconfig.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <optional>

#include "compat/bionic_socket_runtime.h"
#include "mocktail/platform/android_log_observer.h"
#include "runtime/https_json.h"
#include "runtime/server_info.h"
#include "window/window.h"

namespace mocktail::runtime {
namespace {

constexpr std::int64_t kSecondUs = 1000000;
constexpr std::int64_t kInGameSilenceUs = 5 * kSecondUs;
constexpr std::int64_t kNoticeUs = 6 * kSecondUs;
constexpr std::int64_t kNoticeWaitForPlaceUs = 3 * kSecondUs;
constexpr std::int64_t kStatsLogUs = 30 * kSecondUs;
constexpr auto kTick = std::chrono::milliseconds(250);
constexpr auto kFadeTick = std::chrono::milliseconds(16);
constexpr std::int64_t kFadeUs = 200000;
// How long the notice is held past its time when the game is not presenting
// steadily, and how long it may be held in total. A fade that starts on a
// stutter is seen as a jump rather than a transition, which reads as the
// notice blinking away while the game catches up, so the notice waits for the
// game to settle. The total is a backstop: a game that never reaches a steady
// frame rate would otherwise keep the notice up for the whole session.
constexpr std::int64_t kNoticeHoldStepUs = 1 * kSecondUs;
constexpr std::int64_t kNoticeHoldLimitUs = 30 * kSecondUs;
// Two queries this close together are one present, not two: the compositor
// asks whether the layer has content before it asks for the frame. No game
// presents anywhere near this fast.
constexpr std::int64_t kSamePresentGapUs = 1000;

struct Color {
  float r;
  float g;
  float b;
  float a;
};

constexpr Color Hex(std::uint32_t rgb, float alpha = 1.0F) {
  return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0F,
          static_cast<float>((rgb >> 8) & 0xFF) / 255.0F,
          static_cast<float>(rgb & 0xFF) / 255.0F, alpha};
}

// Matches the Roblox top bar capsule.
constexpr Color kPanel = Hex(0x121215, 0.75F);
constexpr Color kInk = Hex(0xFFFFFF);
constexpr Color kMuted = Hex(0xFFFFFF, 0.72F);
constexpr Color kBarOff = Hex(0xFFFFFF, 0.25F);
constexpr Color kGood = Hex(0x3FD47D);
constexpr Color kFair = Hex(0xF5C542);
constexpr Color kPoor = Hex(0xFF5A5F);

std::int64_t MonotonicMicros() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * kSecondUs +
         now.tv_nsec / 1000;
}

struct Shared {
  std::atomic<std::uint64_t> target{0};
  std::atomic<std::int64_t> last_packet_us{0};
  AckPingEstimator ping;
  std::mutex join_mutex;
  std::string join_address;
  std::uint64_t join_generation = 0;
  // The server the host joined to, when it knows the id. Roblox answers for it
  // with its own data center, which is more precise than a geolocation of the
  // address, so it is asked first.
  std::string join_server_id;
  std::uint64_t server_generation = 0;
};

Shared& State() {
  static Shared* shared = new Shared();
  return *shared;
}

std::uint64_t TargetKey(std::uint32_t network_address, std::uint16_t port) {
  return (static_cast<std::uint64_t>(network_address) << 16) | port;
}

void ObserveLog(int, const char*, const char* message) {
  if (message == nullptr) {
    return;
  }
  const std::optional<GameServerEndpoint> endpoint =
      ParseGameServerLog(message);
  if (!endpoint.has_value()) {
    return;
  }
  in_addr address{};
  if (inet_pton(AF_INET, endpoint->address.c_str(), &address) != 1) {
    return;
  }
  Shared& shared = State();
  shared.ping.Reset();
  shared.last_packet_us.store(0, std::memory_order_relaxed);
  shared.target.store(TargetKey(address.s_addr, endpoint->port),
                      std::memory_order_release);
  std::lock_guard<std::mutex> lock(shared.join_mutex);
  shared.join_address = endpoint->address;
  ++shared.join_generation;
}

void ObserveDatagram(bool outgoing, const sockaddr* peer,
                     socklen_t peer_length, ssize_t bytes) {
  if (peer->sa_family != AF_INET || peer_length < sizeof(sockaddr_in)) {
    return;
  }
  Shared& shared = State();
  const std::uint64_t target = shared.target.load(std::memory_order_acquire);
  const auto* address = reinterpret_cast<const sockaddr_in*>(peer);
  if (target == 0 ||
      TargetKey(address->sin_addr.s_addr, ntohs(address->sin_port)) !=
          target) {
    return;
  }
  const std::int64_t now = MonotonicMicros();
  if (outgoing) {
    shared.ping.OnSend(now);
    return;
  }
  shared.ping.OnReceive(now, static_cast<std::size_t>(bytes));
  shared.last_packet_us.store(now, std::memory_order_relaxed);
}

std::mutex g_instance_mutex;
ServerInfoOverlay* g_instance = nullptr;
std::atomic<bool> g_may_present{false};

class Canvas {
 public:
  Canvas(int width, int height)
      : width_(width), height_(height),
        pixels_(static_cast<std::size_t>(width) * height * 4, 0.0F) {}

  int width() const { return width_; }
  int height() const { return height_; }

  void Blend(int x, int y, const Color& color, float coverage) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    const float alpha = color.a * std::clamp(coverage, 0.0F, 1.0F);
    if (alpha <= 0.0F) {
      return;
    }
    float* pixel = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    pixel[0] = color.r * alpha + pixel[0] * (1.0F - alpha);
    pixel[1] = color.g * alpha + pixel[1] * (1.0F - alpha);
    pixel[2] = color.b * alpha + pixel[2] * (1.0F - alpha);
    pixel[3] = alpha + pixel[3] * (1.0F - alpha);
  }

  template <typename Distance>
  void Fill(float left, float top, float right, float bottom,
            const Color& color, Distance distance) {
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(right)));
    const int y1 = std::min(height_, static_cast<int>(std::ceil(bottom)));
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        Blend(x, y, color, 0.5F - distance(x + 0.5F, y + 0.5F));
      }
    }
  }

  std::vector<std::uint8_t> StraightRgba() const {
    std::vector<std::uint8_t> bytes(pixels_.size());
    for (std::size_t index = 0; index < pixels_.size(); index += 4) {
      const float alpha = pixels_[index + 3];
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const float straight =
            alpha > 0.0F ? pixels_[index + channel] / alpha : 0.0F;
        bytes[index + channel] = static_cast<std::uint8_t>(
            std::lround(std::clamp(straight, 0.0F, 1.0F) * 255.0F));
      }
      bytes[index + 3] =
          static_cast<std::uint8_t>(std::lround(alpha * 255.0F));
    }
    return bytes;
  }

 private:
  int width_;
  int height_;
  std::vector<float> pixels_;
};

float RoundedBox(float x, float y, float center_x, float center_y,
                 float half_width, float half_height, float radius) {
  const float qx = std::fabs(x - center_x) - half_width + radius;
  const float qy = std::fabs(y - center_y) - half_height + radius;
  return std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F)) +
         std::min(std::max(qx, qy), 0.0F) - radius;
}

float Circle(float x, float y, float center_x, float center_y, float radius) {
  return std::hypot(x - center_x, y - center_y) - radius;
}

float Triangle(float x, float y, const float (&points)[3][2]) {
  float distance = std::numeric_limits<float>::max();
  float sign = 1.0F;
  for (int index = 0, previous = 2; index < 3; previous = index++) {
    const float edge_x = points[previous][0] - points[index][0];
    const float edge_y = points[previous][1] - points[index][1];
    const float to_x = x - points[index][0];
    const float to_y = y - points[index][1];
    const float along = std::clamp(
        (to_x * edge_x + to_y * edge_y) / (edge_x * edge_x + edge_y * edge_y),
        0.0F, 1.0F);
    const float off_x = to_x - edge_x * along;
    const float off_y = to_y - edge_y * along;
    distance = std::min(distance, off_x * off_x + off_y * off_y);
    const bool above = y >= points[index][1];
    const bool below = y < points[previous][1];
    const bool left = edge_x * to_y > edge_y * to_x;
    if ((above && below && left) || (!above && !below && !left)) {
      sign = -sign;
    }
  }
  return sign * std::sqrt(distance);
}

struct Fonts {
  TTF_Font* bold = nullptr;
  float scale = 0.0F;

  void Close() {
    if (bold != nullptr) {
      TTF_CloseFont(bold);
    }
    bold = nullptr;
    scale = 0.0F;
  }
};

std::string FindFont(const char* family, int weight) {
  FcPattern* pattern = FcPatternCreate();
  if (pattern == nullptr) {
    return {};
  }
  FcPatternAddString(pattern, FC_FAMILY,
                     reinterpret_cast<const FcChar8*>(family));
  FcPatternAddInteger(pattern, FC_WEIGHT, weight);
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
  FcPatternDestroy(pattern);
  std::string path;
  FcChar8* file = nullptr;
  if (matched != nullptr &&
      FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch &&
      file != nullptr) {
    path = reinterpret_cast<const char*>(file);
  }
  if (matched != nullptr) {
    FcPatternDestroy(matched);
  }
  return path;
}

std::string RobloxFont(const char* file, int fallback_weight) {
  const char* content = std::getenv("MOCKTAIL_ASSET_PATH");
  if (content != nullptr && content[0] != '\0') {
    const std::string path = std::string(content) + "/fonts/" + file;
    if (access(path.c_str(), R_OK) == 0) {
      return path;
    }
  }
  return FindFont("sans-serif", fallback_weight);
}

bool OpenFonts(Fonts* fonts, float scale) {
  if (fonts->bold != nullptr && fonts->scale == scale) {
    return true;
  }
  fonts->Close();
  static const std::string bold_file =
      RobloxFont("BuilderSans-Bold.otf", FC_WEIGHT_BOLD);
  if (bold_file.empty()) {
    return false;
  }
  fonts->bold = TTF_OpenFont(bold_file.c_str(), 15.0F * scale);
  if (fonts->bold == nullptr) {
    return false;
  }
  fonts->scale = scale;
  return true;
}

int TextWidth(TTF_Font* font, const std::string& text) {
  int width = 0;
  int height = 0;
  if (!TTF_GetStringSize(font, text.c_str(), 0, &width, &height)) {
    return 0;
  }
  return width;
}

void DrawText(Canvas* canvas, TTF_Font* font, const std::string& text,
              float left, float center_y, const Color& color) {
  SDL_Surface* rendered =
      TTF_RenderText_Blended(font, text.c_str(), 0, SDL_Color{255, 255, 255, 255});
  if (rendered == nullptr) {
    return;
  }
  SDL_Surface* surface = SDL_ConvertSurface(rendered, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(rendered);
  if (surface == nullptr) {
    return;
  }
  const int top = static_cast<int>(std::lround(center_y - surface->h / 2.0F));
  const int x0 = static_cast<int>(std::lround(left));
  const auto* pixels = static_cast<const std::uint8_t*>(surface->pixels);
  for (int y = 0; y < surface->h; ++y) {
    const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * surface->pitch;
    for (int x = 0; x < surface->w; ++x) {
      canvas->Blend(x0 + x, top + y, color, row[x * 4 + 3] / 255.0F);
    }
  }
  SDL_DestroySurface(surface);
}

// Cuts a line that is too wide for the card, rather than letting it run off
// the screen. The cut is made between characters, so a multi-byte name is not
// left half drawn.
std::string FitText(TTF_Font* font, const std::string& text, int maximum) {
  if (font == nullptr || maximum <= 0 || TextWidth(font, text) <= maximum) {
    return text;
  }
  std::size_t length = text.size();
  while (length > 0) {
    // A continuation byte belongs to the character before it, so the cut goes
    // back over those.
    while (length > 0 &&
           (static_cast<unsigned char>(text[length - 1]) & 0xC0) == 0x80) {
      --length;
    }
    const std::string candidate = text.substr(0, length) + "…";
    if (TextWidth(font, candidate) <= maximum) {
      return candidate;
    }
    --length;
  }
  return "…";
}

struct Card {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
};

Card Finish(const Canvas& canvas) {
  return {canvas.StraightRgba(), canvas.width(), canvas.height()};
}

void DrawCapsule(Canvas* canvas, float width, float height,
                 const Color& panel) {
  const float center_y = height / 2.0F;
  canvas->Fill(0, 0, width, height, panel, [&](float x, float y) {
    return RoundedBox(x, y, width / 2.0F, center_y, width / 2.0F,
                      height / 2.0F, height / 2.0F);
  });
}

std::string ReadingText(int value, const char* unit) {
  return (value >= 0 ? std::to_string(value) : std::string("--")) + unit;
}

std::optional<Card> DrawPopup(Fonts* fonts, float scale,
                              const std::string& place,
                              const std::string& source, int view_width) {
  if (!OpenFonts(fonts, scale)) {
    return std::nullopt;
  }
  // The notice is a single line, and the line is the message. Where the answer
  // came from is kept to a short tag: it is worth knowing that the host asked
  // Roblox rather than a third party, but a sentence about it would crowd out
  // the place the player came for.
  const std::string label = source.empty()
                                 ? place
                                 : "Server location: " + place + " (" +
                                       source + ")";
  const float unit = scale;
  const float height = std::round(44.0F * unit);
  const float pad_left = 14.0F * unit;
  const float pad_right = 18.0F * unit;
  const float pin_width = 20.0F * unit;
  const float pin_gap = 6.0F * unit;
  // The card takes at most three quarters of the window, so a long place name
  // is cut rather than drawn off the screen.
  const int maximum_text = std::max(
      0, static_cast<int>(view_width * 0.75F) -
             static_cast<int>(pad_left + pin_width + pin_gap + pad_right));
  const std::string fitted = FitText(fonts->bold, label, maximum_text);
  const float title_width =
      static_cast<float>(TextWidth(fonts->bold, fitted));
  const float width =
      std::ceil(pad_left + pin_width + pin_gap + title_width + pad_right);
  Canvas canvas(static_cast<int>(width), static_cast<int>(height));
  const float center_y = height / 2.0F;
  DrawCapsule(&canvas, width, height, kPanel);

  float cursor = pad_left;
  const float pin_x = cursor + pin_width / 2.0F;
  const float head_y = center_y - 2.5F * unit;
  const float tip[3][2] = {{pin_x - 5.0F * unit, head_y + 2.2F * unit},
                           {pin_x + 5.0F * unit, head_y + 2.2F * unit},
                           {pin_x, center_y + 8.0F * unit}};
  canvas.Fill(cursor - 1, center_y - 10.0F * unit, cursor + pin_width + 1,
              center_y + 10.0F * unit, kInk, [&](float x, float y) {
                const float pin = std::min(
                    Circle(x, y, pin_x, head_y, 5.6F * unit),
                    Triangle(x, y, tip));
                return std::max(pin,
                                -Circle(x, y, pin_x, head_y, 2.2F * unit));
              });
  cursor += pin_width + pin_gap;
  DrawText(&canvas, fonts->bold, fitted, cursor, center_y, kInk);
  return Finish(canvas);
}

std::optional<Card> DrawHud(Fonts* fonts, float scale, bool show_fps, int fps,
                            bool show_ping, int ping_ms) {
  if (!OpenFonts(fonts, scale)) {
    return std::nullopt;
  }
  const float unit = scale;
  const float height = std::round(44.0F * unit);
  const float pad = 16.0F * unit;
  const float group_gap = 16.0F * unit;
  const float bars_width = 14.0F * unit;
  const float bars_gap = 6.0F * unit;
  const std::string fps_text = ReadingText(fps, " fps");
  const std::string ping_text = ReadingText(ping_ms, " ms");
  const float fps_text_width =
      static_cast<float>(TextWidth(fonts->bold, fps_text));
  const float ping_text_width =
      static_cast<float>(TextWidth(fonts->bold, ping_text));
  const float fps_width = std::max(
      fps_text_width, static_cast<float>(TextWidth(fonts->bold, "999 fps")));
  const float ping_width = std::max(
      ping_text_width, static_cast<float>(TextWidth(fonts->bold, "999 ms")));
  float width = pad * 2.0F;
  if (show_fps) {
    width += fps_width;
  }
  if (show_ping) {
    width += bars_width + bars_gap + ping_width;
  }
  if (show_fps && show_ping) {
    width += group_gap;
  }
  width = std::ceil(width);
  Canvas canvas(static_cast<int>(width), static_cast<int>(height));
  const float center_y = height / 2.0F;
  DrawCapsule(&canvas, width, height, kPanel);

  float cursor = pad;
  if (show_fps) {
    DrawText(&canvas, fonts->bold, fps_text,
             cursor + (fps_width - fps_text_width) / 2.0F, center_y, kInk);
    cursor += fps_width + group_gap;
  }
  if (!show_ping) {
    return Finish(canvas);
  }

  Color ping_color = kMuted;
  int lit_bars = 0;
  if (ping_ms >= 0) {
    switch (RatePing(ping_ms)) {
      case PingRating::kGood:
        ping_color = kGood;
        lit_bars = 3;
        break;
      case PingRating::kFair:
        ping_color = kFair;
        lit_bars = 2;
        break;
      case PingRating::kPoor:
        ping_color = kPoor;
        lit_bars = 1;
        break;
    }
  }
  const float bar_heights[3] = {4.5F, 8.0F, 12.0F};
  const float bar_bottom = center_y + 6.0F * unit;
  const float ping_slack = (ping_width - ping_text_width) / 2.0F;
  cursor += ping_slack;
  for (int bar = 0; bar < 3; ++bar) {
    const float bar_width = 3.5F * unit;
    const float bar_left = cursor + bar * 5.25F * unit;
    const float bar_height = bar_heights[bar] * unit;
    canvas.Fill(bar_left - 1, bar_bottom - bar_height - 1,
                bar_left + bar_width + 1, bar_bottom + 1,
                bar < lit_bars ? ping_color : kBarOff, [&](float x, float y) {
                  return RoundedBox(x, y, bar_left + bar_width / 2.0F,
                                    bar_bottom - bar_height / 2.0F,
                                    bar_width / 2.0F, bar_height / 2.0F,
                                    bar_width / 2.0F);
                });
  }
  cursor += bars_width + bars_gap;
  DrawText(&canvas, fonts->bold, ping_text, cursor, center_y, kInk);
  return Finish(canvas);
}

float Eased(float progress) {
  return progress * progress * (3.0F - 2.0F * progress);
}

void Blit(const Card& card, const OverlayRect& at, const OverlayRect& bounds,
          float opacity, std::vector<std::uint8_t>* rgba) {
  const float eased = Eased(opacity);
  const int left = at.x - bounds.x;
  const int top = at.y - bounds.y;
  for (int y = 0; y < card.height; ++y) {
    const int row = top + y;
    if (row < 0 || row >= bounds.height) {
      continue;
    }
    for (int x = 0; x < card.width; ++x) {
      const int column = left + x;
      if (column < 0 || column >= bounds.width) {
        continue;
      }
      const std::uint8_t* source =
          &card.rgba[(static_cast<std::size_t>(y) * card.width + x) * 4];
      std::uint8_t* target =
          &(*rgba)[(static_cast<std::size_t>(row) * bounds.width + column) * 4];
      std::memcpy(target, source, 3);
      target[3] = static_cast<std::uint8_t>(std::lround(source[3] * eased));
    }
  }
}

// What a lookup answered, and where it came from.
struct LookupResult {
  // The summary to show, empty when the lookup found nothing.
  std::string summary;
  // "Roblox" for its own data center, "ipinfo.io" for the address lookup.
  std::string source;
  // The server the answer belongs to: the id for Roblox, the address for the
  // address lookup.
  std::string subject;
};

LookupResult LookupPlace(const std::string& address,
                        const std::atomic<bool>* cancel) {
  const std::optional<nlohmann::json> reply =
      GetHttpsJson("https://ipinfo.io/" + address + "/json", cancel);
  LookupResult result;
  if (reply.has_value()) {
    result.summary = ParseIpInfoPlace(*reply);
  }
  return result;
}

// Roblox's own answer for a server: the data center's region code, such as
// "IAD". It is the only source that knows Roblox's data centers, and asking
// here keeps the session off any third party.
LookupResult LookupRobloxRegion(const std::string& server_id,
                                const std::atomic<bool>* cancel) {
  const std::optional<nlohmann::json> reply =
      GetHttpsJson(BuildRobloxServerUrl(server_id), cancel);
  LookupResult result;
  if (reply.has_value()) {
    const std::optional<std::string> region = ParseRobloxRegion(*reply);
    if (region.has_value()) {
      result.summary = *region;
    }
  }
  return result;
}

}

namespace {

void LogSettings(const OverlayConfig& config) {
  std::fprintf(stderr,
               "  [server-info] location %s, fps %s, ping %s, stats at %s\n",
               config.location ? "on" : "off", config.fps ? "on" : "off",
               config.ping ? "on" : "off",
               std::string(OverlayPositionName(config.position)).c_str());
}

}

bool ServerInfoOverlay::Content::operator==(const Content& other) const {
  return visible == other.visible && show_popup == other.show_popup &&
         show_hud == other.show_hud && popup_alpha == other.popup_alpha &&
         hud_alpha == other.hud_alpha && place == other.place &&
         source == other.source && ping_ms == other.ping_ms &&
         fps == other.fps && show_fps == other.show_fps &&
         show_ping == other.show_ping && position == other.position &&
         location_offset == other.location_offset &&
         pixel_width == other.pixel_width &&
         pixel_height == other.pixel_height && scale == other.scale;
}

void ServerInfoOverlay::RecordPresent(std::int64_t now_us) {
  std::lock_guard<std::mutex> lock(pacing_mutex_);
  // The same present asked twice: the second query is not another frame.
  if (last_present_us_ != 0 && now_us - last_present_us_ < kSamePresentGapUs) {
    return;
  }
  last_present_us_ = now_us;
  presents_us_[present_next_] = now_us;
  present_next_ = (present_next_ + 1) % kPacingSamples;
  if (present_count_ < kPacingSamples) {
    ++present_count_;
  }
}

bool ServerInfoOverlay::PresentsAreSteady() const {
  std::lock_guard<std::mutex> lock(pacing_mutex_);
  if (present_count_ < kPacingSamples) {
    return false;
  }
  // present_next_ points at the oldest kept present once the ring is full, so
  // walking from it yields the intervals in the order they happened.
  std::array<std::int64_t, kPacingSamples> intervals{};
  std::int64_t previous = 0;
  for (std::size_t index = 0; index < kPacingSamples; ++index) {
    const std::int64_t present =
        presents_us_[(present_next_ + index) % kPacingSamples];
    if (index > 0) {
      intervals[index - 1] = present - previous;
    }
    previous = present;
  }
  return PresentIntervalsAreSteady(intervals.data(), kPacingSamples - 1);
}
ServerInfoOverlay::ServerInfoOverlay(OverlayConfig config,
                                     std::filesystem::path config_file)
    : config_(config), config_file_(std::move(config_file)) {}

ServerInfoOverlay::~ServerInfoOverlay() { Stop(); }

void ServerInfoOverlay::Start() {
  if (started_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    if (g_instance != nullptr) {
      return;
    }
    g_instance = this;
  }
  started_ = true;
  stopping_ = false;
  cancel_lookup_.store(false, std::memory_order_relaxed);
  (void)mocktail_android_log_add_observer(&ObserveLog);
  mocktail_bionic_set_datagram_observer(&ObserveDatagram);
  worker_ = std::thread(&ServerInfoOverlay::Run, this);
  LogSettings(config_);
}

void ServerInfoOverlay::Stop() {
  if (!started_) {
    return;
  }
  mocktail_android_log_remove_observer(&ObserveLog);
  mocktail_bionic_set_datagram_observer(nullptr);
  cancel_lookup_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(wait_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  g_may_present.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == this) {
    g_instance = nullptr;
  }
  started_ = false;
}

void ServerInfoOverlay::ObserveJoinedServer(std::string game_instance_id) {
  if (game_instance_id.empty()) {
    return;
  }
  Shared& shared = State();
  std::lock_guard<std::mutex> lock(shared.join_mutex);
  if (shared.join_server_id == game_instance_id) {
    return;
  }
  shared.join_server_id = game_instance_id;
  ++shared.server_generation;
}

void ServerInfoOverlay::Run() {
  const bool ttf_ready = TTF_Init();
  Fonts fonts;
  Shared& shared = State();
  // The answers, each with the server it belongs to: Roblox's own are keyed by
  // the server id, the address lookups by the address. An answer is kept so a
  // second join to the same server does not ask a third party again.
  std::map<std::string, std::string> roblox_places;
  std::map<std::string, std::string> address_places;
  std::uint64_t seen_generation = 0;
  std::uint64_t seen_server_generation = 0;
  std::string address;
  std::string server_id;
  std::string place;
  std::string source;
  std::int64_t joined_at = 0;
  std::int64_t notice_until = 0;
  std::int64_t notice_expired_at = 0;
  bool notice_armed = false;
  bool place_pending = false;
  bool roblox_done = false;
  std::future<LookupResult> lookup;
  std::string lookup_subject;
  std::string lookup_source;
  std::uint64_t frames_before = window::PresentedFrameCount();
  std::int64_t frames_at = MonotonicMicros();
  int fps = -1;
  std::int64_t logged_at = 0;
  OverlayConfig config = config_;
  OverlayConfigReloader reloader(config_file_, config);
  bool hud_fps = config.fps;
  bool hud_ping = config.ping;
  float popup_opacity = 0.0F;
  float hud_opacity = 0.0F;
  bool fading = false;
  std::int64_t faded_at = MonotonicMicros();
  Content popup_drawn;
  std::optional<Card> popup_card;
  Content hud_drawn;
  std::optional<Card> hud_card;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wait_mutex_);
      if (wake_.wait_for(lock, fading ? kFadeTick : kTick,
                         [this] { return stopping_; })) {
        break;
      }
    }
    std::int64_t now = MonotonicMicros();
    std::string reload_error;
    if (const std::optional<OverlayConfig> changed =
            reloader.Reload(config, &reload_error)) {
      config = *changed;
      LogSettings(config);
    } else if (!reload_error.empty()) {
      std::fprintf(stderr, "  [server-info] config not applied: %s\n",
                   reload_error.c_str());
    }
    const bool looks_up = config.location;
    const bool shows_hud = config.fps || config.ping;
    if (shows_hud) {
      hud_fps = config.fps;
      hud_ping = config.ping;
    }
    {
      std::lock_guard<std::mutex> lock(shared.join_mutex);
      if (shared.join_generation != seen_generation) {
        seen_generation = shared.join_generation;
        address = shared.join_address;
        joined_at = now;
        notice_armed = true;
        place_pending = !address.empty() && looks_up;
        place.clear();
        source.clear();
        roblox_done = false;
      }
      if (shared.server_generation != seen_server_generation) {
        seen_server_generation = shared.server_generation;
        server_id = shared.join_server_id;
        roblox_done = false;
      }
    }
    // Roblox first: it is the only source that knows its own data centers, and
    // answering here keeps the session off any third-party service. The
    // address lookup waits for it, so a server Roblox can place is never sent
    // to a third party.
    const bool roblox_pending = lookup.valid() && lookup_source == "Roblox";
    if (looks_up && !server_id.empty() && !roblox_done && !roblox_pending &&
        place.empty() && !lookup.valid()) {
      const auto cached = roblox_places.find(server_id);
      if (cached != roblox_places.end()) {
        place = cached->second;
        source = "Roblox";
      } else {
        lookup_subject = server_id;
        lookup_source = "Roblox";
        lookup = std::async(std::launch::async, LookupRobloxRegion, server_id,
                            &cancel_lookup_);
      }
    }
    if (lookup.valid() && lookup.wait_for(std::chrono::seconds(0)) ==
                              std::future_status::ready) {
      LookupResult found = lookup.get();
      if (cancel_lookup_.load(std::memory_order_relaxed)) {
        break;
      }
      found.source = lookup_source;
      found.subject = lookup_subject;
      if (found.source == "Roblox") {
        roblox_done = true;
        if (!found.summary.empty()) {
          roblox_places.emplace(found.subject, found.summary);
        }
      } else if (!found.summary.empty()) {
        address_places.emplace(found.subject, found.summary);
      }
      std::fprintf(stderr, "  [server-info] server=%s provider=%s location=%s\n",
                   found.subject.c_str(), found.source.c_str(),
                   found.summary.empty() ? "unknown" : found.summary.c_str());
      if (found.source == "Roblox" && found.subject == server_id) {
        place = found.summary;
        source = found.source;
      } else if (found.source != "Roblox" && found.subject == address) {
        place = found.summary;
        source = found.source;
      }
    }
    if (place_pending && !lookup.valid() && place.empty() &&
        (server_id.empty() || roblox_done)) {
      place_pending = false;
      const auto cached = address_places.find(address);
      if (cached != address_places.end()) {
        place = cached->second;
        source = "ipinfo.io";
      } else {
        lookup_subject = address;
        lookup_source = "ipinfo.io";
        lookup = std::async(std::launch::async, LookupPlace, address,
                            &cancel_lookup_);
      }
    }
    if (now - frames_at >= kSecondUs) {
      const std::uint64_t frames = window::PresentedFrameCount();
      fps = static_cast<int>(std::lround(
          static_cast<double>(frames - frames_before) * kSecondUs /
          static_cast<double>(now - frames_at)));
      frames_before = frames;
      frames_at = now;
    }
    const std::int64_t last_packet =
        shared.last_packet_us.load(std::memory_order_relaxed);
    const bool in_game = last_packet != 0 && now - last_packet < kInGameSilenceUs;
    const std::optional<int> ping = shared.ping.EstimateMs(now);
    if (notice_armed && in_game && looks_up &&
        (!place.empty() || now - joined_at >= kNoticeWaitForPlaceUs)) {
      notice_armed = false;
      if (!place.empty()) {
        notice_until = now + kNoticeUs;
        notice_expired_at = 0;
      }
    }
    // The fade-out waits for the game to present steadily. A fade that starts
    // on a stutter is seen as a jump rather than a transition, which reads as
    // the notice blinking away while the game catches up, so the notice is held
    // until the frame rate settles. The hold is bounded, so a game that never
    // settles does not keep the notice up for the whole session.
    if (notice_until != 0 && now >= notice_until) {
      if (notice_expired_at == 0) {
        notice_expired_at = notice_until;
      }
      const bool held_too_long =
          now - notice_expired_at > kNoticeHoldLimitUs;
      if (!held_too_long && !PresentsAreSteady()) {
        notice_until = now + kNoticeHoldStepUs;
      }
    }
    if (in_game && (looks_up || shows_hud) && now - logged_at >= kStatsLogUs) {
      logged_at = now;
      std::fprintf(stderr, "  [server-info] ping=%d ms fps=%d\n",
                   ping.value_or(-1), fps);
    }

    const window::WindowViewportSnapshot viewport =
        window::GetWindowViewportSnapshot();
    const bool can_show = ttf_ready && viewport.valid();
    const bool want_popup = can_show && looks_up && now < notice_until;
    const bool want_hud = can_show && shows_hud && in_game;
    if (!can_show) {
      popup_opacity = 0.0F;
      hud_opacity = 0.0F;
    }
    // A fade that starts after an idle tick advances by one fade frame, not
    // by the whole idle wait.
    const std::int64_t elapsed =
        fading ? now - faded_at
               : std::chrono::duration_cast<std::chrono::microseconds>(
                     kFadeTick)
                     .count();
    popup_opacity = StepFade(popup_opacity, want_popup, elapsed, kFadeUs);
    hud_opacity = StepFade(hud_opacity, want_hud, elapsed, kFadeUs);
    faded_at = now;
    fading = popup_opacity != (want_popup ? 1.0F : 0.0F) ||
             hud_opacity != (want_hud ? 1.0F : 0.0F);

    Content content;
    content.show_popup = popup_opacity > 0.0F;
    content.show_hud = hud_opacity > 0.0F;
    content.popup_alpha =
        static_cast<int>(std::lround(popup_opacity * 255.0F));
    content.hud_alpha = static_cast<int>(std::lround(hud_opacity * 255.0F));
    content.visible = content.show_popup || content.show_hud;
    if (content.visible) {
      if (content.show_popup) {
        content.place = place;
        content.source = source;
      }
      if (content.show_hud) {
        content.show_fps = hud_fps;
        content.show_ping = hud_ping;
        content.fps = hud_fps ? fps : -1;
        content.ping_ms = hud_ping ? ping.value_or(-1) : -1;
        content.position = config.position;
      }
      content.location_offset = config.location_offset;
      content.pixel_width = viewport.pixel_width;
      content.pixel_height = viewport.pixel_height;
      content.scale = static_cast<float>(viewport.pixel_width) /
                      static_cast<float>(viewport.logical_width);
    }
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (content == shown_) {
        continue;
      }
    }
    if (!content.visible) {
      Hide(content);
      continue;
    }
    if (content.show_popup &&
        (!popup_card.has_value() || popup_drawn.place != content.place ||
         popup_drawn.source != content.source ||
         popup_drawn.scale != content.scale)) {
      popup_card = DrawPopup(&fonts, content.scale, content.place,
                             content.source, content.pixel_width);
      popup_drawn = content;
    }
    if (content.show_hud &&
        (!hud_card.has_value() || hud_drawn.fps != content.fps ||
         hud_drawn.ping_ms != content.ping_ms ||
         hud_drawn.show_fps != content.show_fps ||
         hud_drawn.show_ping != content.show_ping ||
         hud_drawn.scale != content.scale)) {
      hud_card = DrawHud(&fonts, content.scale, content.show_fps, content.fps,
                         content.show_ping, content.ping_ms);
      hud_drawn = content;
    }
    if ((content.show_popup && !popup_card.has_value()) ||
        (content.show_hud && !hud_card.has_value())) {
      content.visible = false;
      popup_opacity = 0.0F;
      hud_opacity = 0.0F;
      fading = false;
      Hide(content);
      continue;
    }
    const OverlayLayout layout = LayOutOverlay(
        content.position, content.pixel_width, content.pixel_height,
        content.show_popup ? OverlaySize{popup_card->width, popup_card->height}
                           : OverlaySize{},
        content.show_hud ? OverlaySize{hud_card->width, hud_card->height}
                         : OverlaySize{},
        content.scale,
        Eased(popup_opacity), content.location_offset);
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(layout.bounds.width) * layout.bounds.height *
            4,
        0);
    if (content.show_popup) {
      Blit(*popup_card, layout.popup, layout.bounds, popup_opacity, &rgba);
    }
    if (content.show_hud) {
      Blit(*hud_card, layout.hud, layout.bounds, hud_opacity, &rgba);
    }
    const OverlayRect at = layout.bounds;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    shown_ = content;
    rgba_ = std::move(rgba);
    ++revision_;
    frame_ = {};
    frame_.visible = 1;
    frame_.revision = kRevisionBit | revision_;
    frame_.coordinate_width = static_cast<std::uint32_t>(content.pixel_width);
    frame_.coordinate_height = static_cast<std::uint32_t>(content.pixel_height);
    frame_.x = at.x;
    frame_.y = at.y;
    frame_.width = static_cast<std::uint32_t>(at.width);
    frame_.height = static_cast<std::uint32_t>(at.height);
    frame_.row_bytes = frame_.width * 4U;
    frame_.rgba_bytes = static_cast<std::uint64_t>(frame_.row_bytes) *
                        frame_.height;
    g_may_present.store(true, std::memory_order_release);
  }
  fonts.Close();
  if (ttf_ready) {
    TTF_Quit();
  }
}

void ServerInfoOverlay::Hide(const Content& content) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  shown_ = content;
  frame_ = {};
  rgba_.clear();
  g_may_present.store(false, std::memory_order_release);
}

bool ServerInfoOverlay::MayPresent() {
  return g_may_present.load(std::memory_order_acquire);
}

bool ServerInfoOverlay::QueryFrame(MocktailTextOverlayFrameInfo* frame) {
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == nullptr || frame == nullptr) {
    return false;
  }
  // The compositor asks for the layer on every present, which is where the
  // game shows its frame pacing. The notice's fade waits for that pacing to
  // settle, so the presents are noted here.
  g_instance->RecordPresent(MonotonicMicros());
  std::lock_guard<std::mutex> frame_lock(g_instance->frame_mutex_);
  *frame = g_instance->frame_;
  return frame->visible != 0;
}

bool ServerInfoOverlay::CopyFrame(std::uint64_t revision, void* rgba,
                                  std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  if (g_instance == nullptr || rgba == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> frame_lock(g_instance->frame_mutex_);
  const ServerInfoOverlay& self = *g_instance;
  if (self.frame_.visible == 0 || self.frame_.revision != revision ||
      rgba_capacity < self.rgba_.size()) {
    return false;
  }
  std::memcpy(rgba, self.rgba_.data(), self.rgba_.size());
  return true;
}

}
