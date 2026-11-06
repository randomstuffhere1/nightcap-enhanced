#ifndef MOCKTAIL_UPDATE_UPDATE_CONFIG_H_
#define MOCKTAIL_UPDATE_UPDATE_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mocktail::update {

struct UpdateConfig {
  bool automatic = true;
  bool launch_after_update = false;
  std::string source = "apk-pure";
  // Hours a successful "latest version" answer stays good for. A launch inside
  // that window uses the recorded answer instead of asking the provider, which
  // is most of what the startup preflight costs. 0 asks on every launch.
  // false (the default) lets a launch skip re-hashing a payload whose file
  // identity matches its recorded verification. true re-hashes every time.
  bool reverify_library_on_launch = false;
};

struct UpdateConfigResult {
  UpdateConfig config;
  bool file_loaded = false;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Other top-level sections are handled by runtime_config_file.cc.
UpdateConfigResult LoadUpdateConfig(const std::filesystem::path& path);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_UPDATE_CONFIG_H_
