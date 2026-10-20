#ifndef MOCKTAIL_RUNTIME_SERVER_INFO_OVERLAY_H_
#define MOCKTAIL_RUNTIME_SERVER_INFO_OVERLAY_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mocktail/graphics/text_overlay_frame.h"
#include "runtime/runtime_config.h"

namespace mocktail::runtime {

class ServerInfoOverlay final {
 public:
  ServerInfoOverlay(OverlayConfig config, std::filesystem::path config_file);
  ~ServerInfoOverlay();

  ServerInfoOverlay(const ServerInfoOverlay&) = delete;
  ServerInfoOverlay& operator=(const ServerInfoOverlay&) = delete;

  void Start();
  void Stop();

  // The id of the server the session joined, when the host knows it. Roblox
  // answers with the server's own data center, which is more precise than a
  // geolocation of its address, and answering there keeps the session off any
  // third party.
  void ObserveJoinedServer(std::string game_instance_id);

  static bool MayPresent();
  static bool QueryFrame(MocktailTextOverlayFrameInfo* frame);
  static bool CopyFrame(std::uint64_t revision, void* rgba,
                        std::size_t rgba_capacity);

  static constexpr std::uint64_t kRevisionBit = 1ULL << 63;

  // How many consecutive presents are kept to judge the game's frame pacing.
  static constexpr std::size_t kPacingSamples = 6;

 private:
  struct Content {
    bool visible = false;
    bool show_popup = false;
    bool show_hud = false;
    int popup_alpha = 0;
    int hud_alpha = 0;
    std::string place;
    // Where the place came from, named in the label: "Roblox" for its own
    // data center, "ipinfo.io" for the address lookup.
    std::string source;
    int ping_ms = -1;
    int fps = -1;
    bool show_fps = false;
    bool show_ping = false;
    OverlayPosition position = OverlayPosition::kBottomLeft;
    int location_offset = 12;
    int pixel_width = 0;
    int pixel_height = 0;
    float scale = 1.0F;
    bool operator==(const Content& other) const;
  };

  void Run();
  void Hide(const Content& content);
  // Notes a present, so the frame pacing can be judged. The compositor asks
  // for the layer on every present, which is where the game shows its pace.
  void RecordPresent(std::int64_t now_us);
  // Whether the game is presenting frames steadily enough for a fade to be
  // seen as a transition.
  bool PresentsAreSteady() const;

  OverlayConfig config_;
  std::filesystem::path config_file_;
  std::thread worker_;
  std::mutex wait_mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool started_ = false;
  std::atomic<bool> cancel_lookup_{false};

  std::mutex frame_mutex_;
  Content shown_;
  std::vector<std::uint8_t> rgba_;
  MocktailTextOverlayFrameInfo frame_{};
  std::uint64_t revision_ = 0;

  mutable std::mutex pacing_mutex_;
  std::array<std::int64_t, kPacingSamples> presents_us_{};
  std::size_t present_next_ = 0;
  std::size_t present_count_ = 0;
  std::int64_t last_present_us_ = 0;
};

}

#endif
