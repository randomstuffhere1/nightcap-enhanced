#include "runtime/runtime_config_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumRuntimeConfigBytes = 1024U * 1024U;

using ValueMap = std::unordered_map<std::string, std::string>;

class LayeredEnvironment final : public Environment {
 public:
  LayeredEnvironment(const Environment& primary, ValueMap fallback)
      : primary_(primary), fallback_(std::move(fallback)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const std::optional<std::string> primary = primary_.Get(name);
    if (primary.has_value()) {
      return primary;
    }
    const auto found = fallback_.find(std::string(name));
    return found == fallback_.end() ? std::nullopt
                                    : std::optional<std::string>(found->second);
  }

 private:
  const Environment& primary_;
  ValueMap fallback_;
};

std::string Scalar(const yaml_node_t* node) {
  if (node == nullptr || node->type != YAML_SCALAR_NODE) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(node->data.scalar.value),
                     node->data.scalar.length);
}

bool ReadMapping(yaml_document_t* document, const yaml_node_t* mapping,
                 const std::string& section, ValueMap* values,
                 std::string* error) {
  if (mapping == nullptr || mapping->type != YAML_MAPPING_NODE) {
    *error = section + " must be a mapping";
    return false;
  }
  std::unordered_set<std::string> keys;
  for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start;
       pair != mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node = yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    const std::string key = Scalar(key_node);
    if (key.empty() || value_node == nullptr) {
      *error = section + " contains a non-scalar or empty key";
      return false;
    }
    if (!keys.insert(key).second) {
      *error = section + " contains duplicate key: " + key;
      return false;
    }
    const std::string value = Scalar(value_node);
    if (value_node->type != YAML_SCALAR_NODE) {
      *error = section + "." + key + " must be a scalar";
      return false;
    }
    (*values)[section + "." + key] = value;
  }
  return true;
}

bool ReadNestedMapping(yaml_document_t* document, const yaml_node_t* mapping,
                       const std::string& section, int remaining_depth,
                       ValueMap* values, std::string* error) {
  if (mapping == nullptr || mapping->type != YAML_MAPPING_NODE) {
    *error = section + " must be a mapping";
    return false;
  }
  std::unordered_set<std::string> keys;
  for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start;
       pair != mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node = yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    const std::string key = Scalar(key_node);
    if (key.empty() || value_node == nullptr) {
      *error = section + " contains a non-scalar or empty key";
      return false;
    }
    if (!keys.insert(key).second) {
      *error = section + " contains duplicate key: " + key;
      return false;
    }
    const std::string path = section + "." + key;
    if (value_node->type == YAML_SCALAR_NODE) {
      (*values)[path] = Scalar(value_node);
      continue;
    }
    if (value_node->type != YAML_MAPPING_NODE || remaining_depth <= 0) {
      *error = path + " must be a scalar";
      return false;
    }
    if (!ReadNestedMapping(document, value_node, path, remaining_depth - 1,
                           values, error)) {
      return false;
    }
  }
  return true;
}

bool ParseBoolean(std::string_view value, bool* parsed) {
  if (value == "true") {
    *parsed = true;
    return true;
  }
  if (value == "false") {
    *parsed = false;
    return true;
  }
  return false;
}

bool ParsePositiveInt(std::string_view value, int* parsed) {
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, *parsed);
  return result.ec == std::errc() && result.ptr == end && *parsed > 0;
}

// Most configuration keys are the same shape: look one key up, validate it,
// write it into the environment under a known variable. Those are declared as
// data in the k*Fields tables below and walked by ApplyFields, so a key cannot
// be accepted without being handled, or handled without being accepted.
//
// Keys that do not fit stay as explicit code in ValidateAndMap: the ones that
// validate a value through a parser with its own rules (graphics.backend,
// graphics.quality, performance.physics_worker_mode, ...), the ones whose value
// is cross-checked against another key (network.proxy_host with
// network.proxy_port), and the ones that read a nested mapping (device).
enum class FieldKind {
  // Value must be one of `literals`, a space-separated list. Stored verbatim.
  kEnum,
  // Value must parse as an unsigned integer within [minimum, maximum]. Stored
  // verbatim, as the original spelling.
  kBoundedUnsigned,
  // Value must parse as a positive integer. Stored as decimal, which normalises
  // "  7  " and "007" into one spelling.
  kPositiveInt,
  // Strict true/false. Stored as "1"/"0".
  kBooleanDigits,
  // Strict true/false. Stored as "on"/"off".
  kBooleanOnOff,
  // Non-empty, no control bytes, at most `maximum` bytes.
  kBoundedRequiredString,
  // As above, but an empty value is allowed: an empty Discord presence field
  // hides that field rather than failing the load.
  kBoundedOptionalString,
  // No validation at all. Only for values the consumer re-validates itself.
  kPassthrough,
};

// A constructor rather than default member initialisers, so the short rows stay
// short without tripping -Wmissing-field-initializers.
struct Field {
  constexpr Field(std::string_view key, std::string_view variable_name,
                  FieldKind field_kind = FieldKind::kPassthrough,
                  std::uint32_t low = 0, std::uint32_t high = 0,
                  std::string_view allowed = {})
      : yaml(key),
        variable(variable_name),
        kind(field_kind),
        // Low is the lower bound for kBoundedUnsigned, and the byte ceiling for
        // the string kinds.
        minimum(low),
        maximum(high),
        literals(allowed) {}

  std::string_view yaml;
  std::string_view variable;
  FieldKind kind;
  std::uint32_t minimum;
  std::uint32_t maximum;
  std::string_view literals;
};

constexpr std::array<Field, 5> kGraphicsFields{
    Field{"graphics.vsync", "MOCKTAIL_VSYNC", FieldKind::kEnum, 0, 0,
          "auto on off"},
    Field{"graphics.texture_compression", "MOCKTAIL_ETC2_BC_TRANSCODE",
          FieldKind::kBooleanDigits},
    Field{"graphics.small_texture_upscale", "MOCKTAIL_SMALL_TEXTURE_UPSCALE",
          FieldKind::kEnum, 0, 0, "adaptive 1 2 4"},
    Field{"graphics.present_mode", "MOCKTAIL_PRESENT_MODE", FieldKind::kEnum, 0,
          0, "auto fifo mailbox immediate"},
    Field{"graphics.pipeline_cache", "MOCKTAIL_PIPELINE_CACHE_MODE",
          FieldKind::kEnum, 0, 0, "on off clear"},
};

constexpr std::array<Field, 4> kPerformanceFields{
    Field{"performance.texture_memory_mb", "MOCKTAIL_TEXTURE_MEMORY_MB",
          FieldKind::kBoundedUnsigned, 0, 65536},
    Field{"audio.buffer_samples", "MOCKTAIL_AUDIO_BUFFER_SAMPLES",
          FieldKind::kBoundedUnsigned, 0, 8192},
    Field{"performance.multithreaded_rendering",
          "MOCKTAIL_MULTITHREADED_RENDERING", FieldKind::kBooleanDigits},
    Field{"performance.mangohud", "MANGOHUD", FieldKind::kBooleanDigits},
};

constexpr std::array<Field, 5> kWindowAndDeviceFields{
    Field{"audio.output_device", "MOCKTAIL_AUDIO_OUTPUT_DEVICE",
          FieldKind::kBoundedRequiredString, 0, 512},
    Field{"audio.input_device", "MOCKTAIL_AUDIO_INPUT_DEVICE",
          FieldKind::kBoundedRequiredString, 0, 512},
    Field{"window.width", "MOCKTAIL_WIN_WIDTH", FieldKind::kPositiveInt},
    Field{"window.height", "MOCKTAIL_WIN_HEIGHT", FieldKind::kPositiveInt},
    Field{"window.exclusive_fullscreen", "MOCKTAIL_WIN_EXCLUSIVE_FULLSCREEN",
          FieldKind::kBooleanDigits},
};

constexpr std::array<Field, 6> kInputFields{
    Field{"window.high_dpi", "MOCKTAIL_WIN_HIGH_DPI",
          FieldKind::kBooleanDigits},
    Field{"window.close_on_leave", "MOCKTAIL_WIN_CLOSE_ON_LEAVE",
          FieldKind::kBooleanDigits},
    Field{"input.touch_enabled", "MOCKTAIL_TOUCH_MODE",
          FieldKind::kBooleanOnOff},
    Field{"input.raw_mouse", "MOCKTAIL_RAW_MOUSE", FieldKind::kBooleanOnOff},
    Field{"input.pump_hz", "MOCKTAIL_INPUT_PUMP_HZ",
          FieldKind::kBoundedUnsigned, 60, 360},
    Field{"compatibility.desktop_playability",
          "MOCKTAIL_DESKTOP_PLAYABILITY", FieldKind::kBooleanDigits},
};

constexpr std::array<Field, 3> kFleasionPassthroughFields{
    Field{"integrations.fleasion.proxy_mode",
          "MOCKTAIL_FLEASION_PROXY_MODE"},
    Field{"integrations.fleasion.proxy_port", "MOCKTAIL_FLEASION_PROXY_PORT"},
    Field{"integrations.fleasion.ca_certificate",
          "MOCKTAIL_FLEASION_CA_CERTIFICATE"},
};

constexpr std::array<Field, 7> kIntegrationBooleanFields{
    Field{"integrations.discord_rpc.enabled",
          "MOCKTAIL_DISCORD_RPC_ENABLED", FieldKind::kBooleanDigits},
    Field{"integrations.fleasion.enabled", "MOCKTAIL_FLEASION_ENABLED",
          FieldKind::kBooleanDigits},
    Field{"integrations.fleasion.clear_cache_on_launch",
          "MOCKTAIL_FLEASION_CLEAR_CACHE_ON_LAUNCH", FieldKind::kBooleanDigits},
    Field{"integrations.discord_rpc.show_place_name",
          "MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME", FieldKind::kBooleanDigits},
    Field{"integrations.discord_rpc.show_elapsed_time",
          "MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME", FieldKind::kBooleanDigits},
    Field{"integrations.discord_rpc.join.enabled",
          "MOCKTAIL_DISCORD_RPC_JOIN_ENABLED", FieldKind::kBooleanDigits},
    Field{"integrations.discord_rpc.join.public_servers_only",
          "MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY",
          FieldKind::kBooleanDigits},
};

constexpr std::array<Field, 5> kDiscordRequiredStringFields{
    Field{"integrations.discord_rpc.join.button_label",
          "MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
          FieldKind::kBoundedRequiredString, 0, 32},
    Field{"integrations.discord_rpc.text.browsing",
          "MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
          FieldKind::kBoundedRequiredString, 0, 128},
    Field{"integrations.discord_rpc.text.joining",
          "MOCKTAIL_DISCORD_RPC_TEXT_JOINING",
          FieldKind::kBoundedRequiredString, 0, 128},
    Field{"integrations.discord_rpc.text.playing",
          "MOCKTAIL_DISCORD_RPC_TEXT_PLAYING",
          FieldKind::kBoundedRequiredString, 0, 128},
    Field{"integrations.discord_rpc.text.unknown_place",
          "MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
          FieldKind::kBoundedRequiredString, 0, 128},
};

constexpr std::array<Field, 6> kDiscordOptionalStringFields{
    Field{"integrations.discord_rpc.text.title",
          "MOCKTAIL_DISCORD_RPC_TEXT_TITLE",
          FieldKind::kBoundedOptionalString, 0, 128},
    Field{"integrations.discord_rpc.text.state",
          "MOCKTAIL_DISCORD_RPC_TEXT_STATE",
          FieldKind::kBoundedOptionalString, 0, 128},
    Field{"integrations.discord_rpc.images.large",
          "MOCKTAIL_DISCORD_RPC_IMAGE_LARGE", FieldKind::kBoundedOptionalString, 0,
          512},
    Field{"integrations.discord_rpc.images.large_text",
          "MOCKTAIL_DISCORD_RPC_IMAGE_LARGE_TEXT",
          FieldKind::kBoundedOptionalString, 0, 128},
    Field{"integrations.discord_rpc.images.small",
          "MOCKTAIL_DISCORD_RPC_IMAGE_SMALL", FieldKind::kBoundedOptionalString, 0,
          512},
    Field{"integrations.discord_rpc.images.small_text",
          "MOCKTAIL_DISCORD_RPC_IMAGE_SMALL_TEXT",
          FieldKind::kBoundedOptionalString, 0, 128},
};

// Keys accepted but handled as explicit code in ValidateAndMap. Everything else
// comes from the tables above, so the list of accepted keys cannot drift away
// from the list of handled keys.
constexpr std::array<std::string_view, 22> kHandledKeys{
    "version",
    "device",
    "device.__mapping",
    "device.type",
    "device.platform",
    "device.name",
    "device.manufacturer",
    "device.model",
    "device.brand",
    "device.code",
    "device.sku",
    "device.soc_model",
    "device.touch",
    "device.mouse",
    "device.keyboard",
    "runtime.headless",
    "runtime.roblox_library",
    "appearance.theme",
    "graphics.backend",
    "graphics.frame_rate_limit",
    "graphics.quality",
    "performance.physics_worker_mode",
};

template <std::size_t N>
bool Contains(std::string_view key, const std::array<std::string_view, N>& keys) {
  for (const std::string_view candidate : keys) {
    if (candidate == key) return true;
  }
  return false;
}

template <std::size_t N>
bool Contains(std::string_view key, const std::array<Field, N>& fields) {
  for (const Field& field : fields) {
    if (field.yaml == key) return true;
  }
  return false;
}

bool IsKnownKey(std::string_view key) {
  return Contains(key, kHandledKeys) || Contains(key, kGraphicsFields) ||
         Contains(key, kPerformanceFields) ||
         Contains(key, kWindowAndDeviceFields) || Contains(key, kInputFields) ||
         Contains(key, kFleasionPassthroughFields) ||
         Contains(key, kIntegrationBooleanFields) ||
         Contains(key, kDiscordRequiredStringFields) ||
         Contains(key, kDiscordOptionalStringFields) ||
         // Cross-checked as a pair, and the scheme/port rules are the parser's.
         key == "performance.memory_limit_mb" || key == "performance.gamemode" ||
         key == "window.title" || key == "network.use_system_proxy" ||
         key == "network.proxy_host" || key == "network.proxy_port" ||
         key == "network.proxy_scheme" || key == "network.proxy_username" ||
         key == "network.proxy_password" || key == "network.ca_bundle" ||
         key == "input.system_cursor" || key == "input.hardware_cursor" ||
         key == "integrations.discord_rpc.application_id" ||
         // The overlay rows are read as a group, and their rules live in the
         // parser below rather than in a field table.
         key == "overlay.server_info" || key == "overlay.location" ||
         key == "overlay.location_offset" || key == "overlay.fps" ||
         key == "overlay.ping" || key == "overlay.position" ||
         // The fork's own server location indicator, retired in favour of the
         // overlay and kept as a legacy alias so old configs still load.
         key == "integrations.server_location_indicator" ||
         // The fork used to expose the small-texture upscale factor here.
         // Upstream owns the setting and files it under graphics, but a config
         // written by an older build still has to load.
         key == "performance.small_texture_upscale";
}

// "auto, on, or off" from "auto on off". Used for the two enum kinds so the
// rejection message cannot fall out of step with the accepted values.
std::string LiteralList(std::string_view literals) {
  std::vector<std::string_view> parts;
  std::size_t offset = 0;
  while (offset < literals.size()) {
    const std::size_t end = literals.find(' ', offset);
    parts.push_back(literals.substr(
        offset, end == std::string_view::npos ? std::string_view::npos
                                              : end - offset));
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  std::string joined;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      // "a or b" for two, "a, b, or c" from three on.
      joined += index + 1 == parts.size() ? (index == 1 ? " or " : ", or ")
                                         : ", ";
    }
    joined += parts[index];
  }
  return joined;
}

bool LiteralAllowed(std::string_view literals, std::string_view value) {
  std::size_t offset = 0;
  while (offset < literals.size()) {
    const std::size_t end = literals.find(' ', offset);
    if (literals.substr(offset, end == std::string_view::npos
                                        ? std::string_view::npos
                                        : end - offset) == value) {
      return true;
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return false;
}

bool HasControlBytes(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
  });
}

// Validates and writes one key. `serialized` is what goes into the environment,
// which is not always the configured spelling: a boolean becomes 1/0 or on/off
// depending on what the reader expects, and a number is normalised.
bool MapField(const Field& field, std::string_view value, ValueMap* environment,
              std::string* error) {
  const std::string prefix(field.yaml);
  switch (field.kind) {
    case FieldKind::kEnum:
      if (!LiteralAllowed(field.literals, value)) {
        *error = prefix + " must be " + LiteralList(field.literals);
        return false;
      }
      (*environment)[std::string(field.variable)] = std::string(value);
      return true;
    case FieldKind::kBoundedUnsigned: {
      std::uint32_t parsed = 0;
      if (!ParseBoundedUnsigned(value, field.minimum, field.maximum, &parsed)) {
        *error = prefix + " must be " + std::to_string(field.minimum) + " to " +
                 std::to_string(field.maximum);
        return false;
      }
      (*environment)[std::string(field.variable)] = std::string(value);
      return true;
    }
    case FieldKind::kPositiveInt: {
      int parsed = 0;
      if (!ParsePositiveInt(value, &parsed)) {
        *error = prefix + " must be a positive integer";
        return false;
      }
      (*environment)[std::string(field.variable)] = std::to_string(parsed);
      return true;
    }
    case FieldKind::kBooleanDigits:
    case FieldKind::kBooleanOnOff: {
      bool parsed = false;
      if (!ParseBoolean(value, &parsed)) {
        *error = prefix + " must be true or false";
        return false;
      }
      const bool on_off = field.kind == FieldKind::kBooleanOnOff;
      (*environment)[std::string(field.variable)] =
          parsed ? (on_off ? "on" : "1") : (on_off ? "off" : "0");
      return true;
    }
    case FieldKind::kBoundedRequiredString:
    case FieldKind::kBoundedOptionalString: {
      const bool required = field.kind == FieldKind::kBoundedRequiredString;
      if ((required && value.empty()) || value.size() > field.maximum ||
          HasControlBytes(value)) {
        *error = prefix + (required ? " must be non-empty, bounded, and contain"
                                      " no control bytes"
                                    : " must be bounded and contain no control"
                                      " bytes");
        return false;
      }
      (*environment)[std::string(field.variable)] = std::string(value);
      return true;
    }
    case FieldKind::kPassthrough:
      (*environment)[std::string(field.variable)] = std::string(value);
      return true;
  }
  *error = prefix + " is not a supported configuration key";
  return false;
}

template <std::size_t N>
bool ApplyFields(const std::array<Field, N>& fields,
                 const std::unordered_map<std::string, std::string>& yaml,
                 ValueMap* environment, std::string* error) {
  for (const Field& field : fields) {
    const auto found = yaml.find(std::string(field.yaml));
    if (found == yaml.end()) {
      continue;
    }
    if (!MapField(field, found->second, environment, error)) {
      return false;
    }
  }
  return true;
}

bool ValidateAndMap(const ValueMap& yaml, ValueMap* environment,
                    std::string* error) {
  const auto value =
      [&yaml](std::string_view key) -> std::optional<std::string> {
    const auto found = yaml.find(std::string(key));
    return found == yaml.end() ? std::nullopt
                               : std::optional<std::string>(found->second);
  };
  for (const auto& [key, ignored] : yaml) {
    if (!IsKnownKey(key)) {
      *error = "unknown runtime configuration key: " + key;
      return false;
    }
  }

  if (const auto version = value("version");
      version.has_value() && *version != "1") {
    *error = "unsupported runtime configuration version: " + *version;
    return false;
  }
  const auto scalar_device = value("device");
  const bool detailed_device = value("device.__mapping").has_value();
  const auto detailed_type = value("device.type");
  if (detailed_device && !detailed_type.has_value()) {
    *error = "device.type is required for detailed device configuration";
    return false;
  }
  const std::optional<std::string> selected_device =
      scalar_device.has_value() ? scalar_device : detailed_type;
  if (selected_device.has_value()) {
    if (FindDeviceProfile(*selected_device) == nullptr) {
      *error =
          "device must be pc, mobile, console, pc-windows-11, "
          "mobile-pixel-7, or console-ps5";
      return false;
    }
    if (value("input.touch_enabled").has_value() ||
        value("compatibility.desktop_playability").has_value()) {
      *error =
          "device cannot be combined with input.touch_enabled or "
          "compatibility.desktop_playability";
      return false;
    }
    (*environment)["MOCKTAIL_DEVICE_PROFILE"] = *selected_device;
  }
  if (detailed_device) {
    struct StringField {
      std::string_view yaml;
      std::string_view variable;
      std::size_t maximum;
    };
    for (const StringField& field : {
             StringField{"device.platform", "MOCKTAIL_DEVICE_PLATFORM_NAME",
                         128},
             {"device.name", "MOCKTAIL_DEVICE_NAME", 128},
             {"device.manufacturer", "MOCKTAIL_DEVICE_MANUFACTURER", 128},
             {"device.model", "MOCKTAIL_DEVICE_MODEL", 128},
             {"device.brand", "MOCKTAIL_DEVICE_BRAND", 64},
             {"device.code", "MOCKTAIL_DEVICE_CODE", 64},
             {"device.sku", "MOCKTAIL_DEVICE_SKU", 64},
             {"device.soc_model", "MOCKTAIL_DEVICE_SOC_MODEL", 128},
         }) {
      const std::optional<std::string> configured = value(field.yaml);
      if (!configured.has_value()) {
        continue;
      }
      if (!IsValidDeviceProfileValue(*configured, field.maximum)) {
        *error = std::string(field.yaml) +
                 " must be non-empty, bounded, and contain no control bytes";
        return false;
      }
      (*environment)[std::string(field.variable)] = *configured;
    }
    for (const auto& [key, variable] : {
             std::pair<std::string_view, std::string_view>(
                 "device.touch", "MOCKTAIL_TOUCH_MODE"),
             {"device.mouse", "MOCKTAIL_MOUSE_MODE"},
             {"device.keyboard", "MOCKTAIL_KEYBOARD_MODE"},
         }) {
      const std::optional<std::string> configured = value(key);
      if (!configured.has_value()) {
        continue;
      }
      bool parsed = false;
      if (!ParseBoolean(*configured, &parsed)) {
        *error = std::string(key) + " must be true or false";
        return false;
      }
      (*environment)[std::string(variable)] = parsed ? "on" : "off";
    }
  }
  if (const auto headless = value("runtime.headless"); headless.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*headless, &parsed)) {
      *error = "runtime.headless must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_HEADLESS"] = parsed ? "1" : "0";
  }
  if (const auto library = value("runtime.roblox_library");
      library.has_value()) {
    if (library->empty()) {
      *error = "runtime.roblox_library must not be empty";
      return false;
    }
    (*environment)["ROBLOX_LIB_PATH"] = *library;
  }
  if (const auto theme = value("appearance.theme"); theme.has_value()) {
    if (*theme != "roblox" && *theme != "system" && *theme != "light" &&
        *theme != "dark") {
      *error = "appearance.theme must be roblox, system, light, or dark";
      return false;
    }
    (*environment)["MOCKTAIL_THEME"] = *theme;
  }
  if (const auto legacy = value("overlay.server_info"); legacy.has_value()) {
    if (*legacy != "off" && *legacy != "notice" && *legacy != "always") {
      *error = "overlay.server_info must be off, notice, or always";
      return false;
    }
    if (*legacy != "off" && !value("overlay.location").has_value()) {
      (*environment)["MOCKTAIL_OVERLAY_LOCATION"] = "on";
    }
  }
  for (const auto& [key, variable] :
       {std::pair{"overlay.location", "MOCKTAIL_OVERLAY_LOCATION"},
        std::pair{"overlay.fps", "MOCKTAIL_OVERLAY_FPS"},
        std::pair{"overlay.ping", "MOCKTAIL_OVERLAY_PING"}}) {
    if (const auto shown = value(key); shown.has_value()) {
      bool parsed = false;
      if (!ParseBoolean(*shown, &parsed)) {
        *error = std::string(key) + " must be true or false";
        return false;
      }
      (*environment)[variable] = parsed ? "on" : "off";
    }
  }
  // This fork's own server location indicator is retired: the overlay does the
  // same job. A config that still has the old switch keeps loading, and the
  // switch becomes the overlay's own.
  if (const auto retired = value("integrations.server_location_indicator");
      retired.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*retired, &parsed)) {
      *error = "integrations.server_location_indicator must be true or false";
      return false;
    }
    if (parsed && !value("overlay.location").has_value()) {
      (*environment)["MOCKTAIL_OVERLAY_LOCATION"] = "on";
    }
  }
  if (const auto position = value("overlay.position"); position.has_value()) {
    if (!ParseOverlayPosition(*position).has_value()) {
      *error =
          "overlay.position must be top_center, top_right, bottom_left, "
          "bottom_center, or bottom_right";
      return false;
    }
    (*environment)["MOCKTAIL_OVERLAY_POSITION"] = *position;
  }
  if (const auto offset = value("overlay.location_offset"); offset.has_value()) {
    if (!ParseOverlayLocationOffset(*offset).has_value()) {
      *error = "overlay.location_offset must be a whole number from 0 to " +
               std::to_string(kMaximumOverlayLocationOffset);
      return false;
    }
    (*environment)["MOCKTAIL_OVERLAY_LOCATION_OFFSET"] = *offset;
  }
  if (const auto backend = value("graphics.backend"); backend.has_value()) {
    if (RuntimeConfig::ParseGraphicsBackend(*backend) ==
        GraphicsBackend::kUnknown) {
      *error = "graphics.backend is not supported: " + *backend;
      return false;
    }
    (*environment)["MOCKTAIL_GRAPHICS_BACKEND"] = *backend;
  }
  if (const auto frame_rate = value("graphics.frame_rate_limit");
      frame_rate.has_value()) {
    if (!ParseFrameRatePolicy(*frame_rate).valid()) {
      *error = "graphics.frame_rate_limit is not supported: " + *frame_rate;
      return false;
    }
    (*environment)["MOCKTAIL_FRAME_RATE_LIMIT"] = *frame_rate;
  }
  if (!ApplyFields(kGraphicsFields, yaml, environment, error)) {
    return false;
  }
  if (const auto quality = value("graphics.quality"); quality.has_value()) {
    GraphicsQualityConfig parsed;
    if (!ParseGraphicsQuality(*quality, &parsed)) {
      *error = "graphics.quality must be auto or 1 to 6";
      return false;
    }
    (*environment)["MOCKTAIL_GRAPHICS_QUALITY"] = parsed.value();
  }
  if (!ApplyFields(kPerformanceFields, yaml, environment, error)) {
    return false;
  }
  // The fork's old home for the small-texture upscale factor. It keeps its
  // 1-8 range so an existing config behaves as it did, and defers to the
  // graphics key when both are present.
  if (const auto retired = value("performance.small_texture_upscale");
      retired.has_value()) {
    std::uint32_t parsed = 0;
    if (!ParseBoundedUnsigned(*retired, 1, 8, &parsed)) {
      *error = "performance.small_texture_upscale must be 1 to 8";
      return false;
    }
    if (!value("graphics.small_texture_upscale").has_value()) {
      (*environment)["MOCKTAIL_SMALL_TEXTURE_UPSCALE"] = *retired;
    }
  }
  if (const auto physics_worker_mode = value("performance.physics_worker_mode");
      physics_worker_mode.has_value()) {
    PhysicsWorkerMode parsed = PhysicsWorkerMode::kAuto;
    if (!ParsePhysicsWorkerMode(*physics_worker_mode, &parsed)) {
      *error =
          "performance.physics_worker_mode must be auto, latency, or "
          "throughput";
      return false;
    }
    (*environment)["MOCKTAIL_PHYSICS_WORKER_MODE"] =
        std::string(PhysicsWorkerModeName(parsed));
  }
  if (const auto memory_limit = value("performance.memory_limit_mb");
      memory_limit.has_value()) {
    const PerformancePolicy parsed = ParsePerformancePolicy("0", *memory_limit);
    if (!parsed.memory_limit_valid) {
      *error = "performance.memory_limit_mb must be a non-negative integer";
      return false;
    }
    (*environment)["MOCKTAIL_MEMORY_LIMIT_MB"] =
        std::to_string(parsed.memory_limit_mb);
  }
  if (const auto game_mode = value("performance.gamemode");
      game_mode.has_value()) {
    GameModePolicy parsed = GameModePolicy::kAuto;
    if (!ParseGameModePolicy(*game_mode, &parsed)) {
      *error = "performance.gamemode must be auto, on, or off";
      return false;
    }
    (*environment)["MOCKTAIL_GAMEMODE"] = GameModePolicyName(parsed);
  }

  if (!ApplyFields(kWindowAndDeviceFields, yaml, environment, error)) {
    return false;
  }
  if (const auto title = value("window.title"); title.has_value()) {
    if (title->empty()) {
      *error = "window.title must not be empty";
      return false;
    }
    (*environment)["MOCKTAIL_WIN_TITLE"] = *title;
  }
  if (!ApplyFields(kInputFields, yaml, environment, error)) {
    return false;
  }
  if (const auto system_cursor = value("input.system_cursor");
      system_cursor.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*system_cursor, &parsed)) {
      *error = "input.system_cursor must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_SYSTEM_CURSOR"] = parsed ? "on" : "off";
  }
  if (const auto hardware_cursor = value("input.hardware_cursor");
      hardware_cursor.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*hardware_cursor, &parsed)) {
      *error = "input.hardware_cursor must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_HARDWARE_CURSOR"] = parsed ? "on" : "off";
  }
  const auto proxy_host = value("network.proxy_host");
  const auto proxy_port = value("network.proxy_port");
  const auto proxy_scheme = value("network.proxy_scheme");
  const auto proxy_username = value("network.proxy_username");
  const auto proxy_password = value("network.proxy_password");
  bool use_system_proxy = false;
  if (const auto system_proxy = value("network.use_system_proxy");
      system_proxy.has_value()) {
    if (!ParseBoolean(*system_proxy, &use_system_proxy)) {
      *error = "network.use_system_proxy must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_USE_SYSTEM_PROXY"] =
        use_system_proxy ? "1" : "0";
  }
  if (proxy_host.has_value() != proxy_port.has_value() ||
      (proxy_scheme.has_value() && !proxy_host.has_value())) {
    *error =
        "network.proxy_host and network.proxy_port must be configured together";
    return false;
  }
  if (use_system_proxy && proxy_host.has_value()) {
    *error = "network.use_system_proxy cannot be combined with a fixed proxy";
    return false;
  }
  if (proxy_username.has_value() != proxy_password.has_value() ||
      (proxy_username.has_value() &&
       (!proxy_host.has_value() || proxy_username->empty()))) {
    *error = "proxy_username and proxy_password require a fixed proxy "
             "and must be configured together with a nonempty username";
    return false;
  }
  if (proxy_host.has_value()) {
    const auto proxy = ParseNetworkProxyConfig(
        *proxy_host, *proxy_port, proxy_scheme.value_or("http"),
        proxy_username.value_or(""), proxy_password.value_or(""));
    if (!proxy.has_value()) {
      *error =
          "network proxy must use a valid host without a scheme, a port "
          "from 1 to 65535, an HTTP or SOCKS5 scheme, and valid credentials";
      return false;
    }
    (*environment)["MOCKTAIL_HTTP_PROXY_HOST"] = *proxy_host;
    (*environment)["MOCKTAIL_HTTP_PROXY_PORT"] = *proxy_port;
    (*environment)["MOCKTAIL_HTTP_PROXY_SCHEME"] = proxy->scheme;
    (*environment)["MOCKTAIL_HTTP_PROXY_USERNAME"] = proxy->username;
    (*environment)["MOCKTAIL_HTTP_PROXY_PASSWORD"] = proxy->password;
  }
  if (const auto ca_bundle = value("network.ca_bundle");
      ca_bundle.has_value()) {
    const std::filesystem::path path(*ca_bundle);
    if (ca_bundle->empty() || !path.is_absolute()) {
      *error = "network.ca_bundle must be an absolute file path";
      return false;
    }
    (*environment)["MOCKTAIL_CA_BUNDLE"] = *ca_bundle;
  }
  if (!ApplyFields(kIntegrationBooleanFields, yaml, environment, error)) {
    return false;
  }
  if (!ApplyFields(kFleasionPassthroughFields, yaml, environment, error)) {
    return false;
  }
  if (const auto application_id =
          value("integrations.discord_rpc.application_id");
      application_id.has_value()) {
    if (application_id->size() < 17 || application_id->size() > 20 ||
        !std::all_of(application_id->begin(), application_id->end(),
                     [](unsigned char byte) {
                       return byte >= '0' && byte <= '9';
                     })) {
      *error = "integrations.discord_rpc.application_id must be a Discord "
               "snowflake";
      return false;
    }
    (*environment)["MOCKTAIL_DISCORD_APPLICATION_ID"] = *application_id;
  }
  if (!ApplyFields(kDiscordRequiredStringFields, yaml, environment, error)) {
    return false;
  }
  // Presence fields may be empty: an empty value hides that field.
  if (!ApplyFields(kDiscordOptionalStringFields, yaml, environment, error)) {
    return false;
  }
  return true;
}

bool LoadYaml(const std::filesystem::path& path, ValueMap* values, bool* loaded,
              std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      *loaded = false;
      return true;
    }
    *error = "cannot open runtime configuration: " + path.string();
    return false;
  }

  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) >
          kMaximumRuntimeConfigBytes) {
    (void)close(descriptor);
    *error =
        "runtime configuration must be a regular file no larger than 1 MiB";
    return false;
  }

  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 16U * 1024U> buffer = {};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)close(descriptor);
      *error = "cannot read runtime configuration: " + path.string();
      return false;
    }
    if (contents.size() + static_cast<std::size_t>(bytes) >
        kMaximumRuntimeConfigBytes) {
      (void)close(descriptor);
      *error = "runtime configuration exceeds 1 MiB";
      return false;
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  if (close(descriptor) != 0) {
    *error = "cannot close runtime configuration: " + path.string();
    return false;
  }

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    *error = "cannot initialize YAML parser";
    return false;
  }
  yaml_document_t document;
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(contents.data()),
      contents.size());
  const bool parsed = yaml_parser_load(&parser, &document) != 0;
  if (!parsed) {
    *error = "invalid YAML in " + path.string();
    yaml_parser_delete(&parser);
    return false;
  }
  yaml_node_t* root = yaml_document_get_root_node(&document);
  bool valid = true;
  if (root == nullptr || root->type != YAML_MAPPING_NODE) {
    *error = "runtime configuration root must be a mapping";
    valid = false;
  } else {
    std::unordered_set<std::string> top_level_keys;
    for (yaml_node_pair_t* pair = root->data.mapping.pairs.start;
         valid && pair != root->data.mapping.pairs.top; ++pair) {
      const yaml_node_t* key_node =
          yaml_document_get_node(&document, pair->key);
      const yaml_node_t* value_node =
          yaml_document_get_node(&document, pair->value);
      const std::string key = Scalar(key_node);
      if (key.empty() || !top_level_keys.insert(key).second) {
        *error = "runtime configuration has an empty or duplicate key";
        valid = false;
      } else if (key == "version") {
        if (value_node == nullptr || value_node->type != YAML_SCALAR_NODE) {
          *error = "version must be a scalar";
          valid = false;
        } else {
          (*values)["version"] = Scalar(value_node);
        }
      } else if (key == "device") {
        if (value_node != nullptr && value_node->type == YAML_SCALAR_NODE) {
          (*values)["device"] = Scalar(value_node);
        } else if (value_node != nullptr &&
                   value_node->type == YAML_MAPPING_NODE) {
          (*values)["device.__mapping"] = "true";
          valid = ReadMapping(&document, value_node, key, values, error);
        } else {
          *error = "device must be a preset scalar or detailed mapping";
          valid = false;
        }
      } else if (key == "runtime" || key == "appearance" ||
                 key == "graphics" || key == "performance" ||
                 key == "audio" || key == "window" || key == "input" ||
                 key == "compatibility" || key == "network" ||
                 key == "overlay") {
        valid = ReadMapping(&document, value_node, key, values, error);
      } else if (key == "integrations") {
        valid = ReadNestedMapping(&document, value_node, key, 2, values, error);
      }
      // Other top-level sections are owned by subsystems such as the
      // updater. This loader ignores rather than misparses them.
    }
  }
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  *loaded = valid;
  return valid;
}

std::string FrameRateValue(const FrameRatePolicy& policy) {
  if (policy.mode == FrameRateLimitMode::kUnmanaged) {
    return "-1";
  }
  if (policy.mode == FrameRateLimitMode::kUnlimited) {
    return "unlimited";
  }
  if (policy.mode == FrameRateLimitMode::kFixed) {
    return std::to_string(policy.fixed_fps);
  }
  return "display";
}

bool SetEnvironmentValue(const char* name, const std::string& value,
                         std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot export resolved runtime setting: ") + name;
  }
  return false;
}

bool UnsetEnvironmentValue(const char* name, std::string* error) {
  if (unsetenv(name) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot clear resolved runtime setting: ") + name;
  }
  return false;
}

}  // namespace

RuntimeConfigLoadResult LoadRuntimeConfig(
    const Environment& environment, const std::filesystem::path& config_file) {
  RuntimeConfigLoadResult result;
  ValueMap yaml;
  if (!LoadYaml(config_file, &yaml, &result.file_loaded, &result.error)) {
    return result;
  }
  ValueMap fallback;
  if (!yaml.empty() && !ValidateAndMap(yaml, &fallback, &result.error)) {
    return result;
  }
  const LayeredEnvironment layered(environment, std::move(fallback));
  result.config = RuntimeConfig::FromEnvironment(layered);
  if (!result.config.frame_rate().valid()) {
    result.error = "frame-rate policy is invalid";
  } else if (!result.config.device_profile_valid()) {
    result.error = "device profile is invalid";
  } else if (!result.config.input_capabilities().touch_enabled &&
             !result.config.input_capabilities().mouse_enabled &&
             !result.config.input_capabilities().keyboard_enabled) {
    result.error = "device must expose at least one usable input capability";
  } else if (result.config.graphics_backend() == GraphicsBackend::kUnknown) {
    result.error = "graphics backend is invalid";
  } else if (!result.config.theme_mode_valid()) {
    result.error = "theme mode is invalid";
  } else if (!result.config.audio_buffer_valid()) {
    result.error = "audio buffer size is invalid";
  } else if (!result.config.overlay().valid) {
    result.error = "overlay settings are invalid";
  } else if (!result.config.window().high_dpi_valid) {
    result.error = "window high-DPI policy is invalid";
  } else if (!result.config.window().close_on_leave_valid) {
    result.error = "window close-on-leave policy is invalid";
  } else if (!result.config.window().exclusive_fullscreen_valid) {
    result.error = "window exclusive-fullscreen policy is invalid";
  } else if (!result.config.input_capabilities().pump_hz_valid) {
    result.error = "input pump rate must be 60 to 360 Hz";
  } else if (result.config.vsync_mode() != "auto" &&
             result.config.vsync_mode() != "on" &&
             result.config.vsync_mode() != "off") {
    result.error = "VSync policy is invalid";
  } else if (!RuntimeConfig::IsValidPresentMode(result.config.present_mode())) {
    result.error =
        "graphics.present_mode must be auto, fifo, mailbox, or immediate";
  } else if (!result.config.audio_output_device_valid()) {
    result.error = "audio output device is invalid";
  } else if (!result.config.audio_input_device_valid()) {
    result.error = "audio input device is invalid";
  } else if (!result.config.performance().memory_limit_valid) {
    result.error = "memory-limit policy is invalid";
  } else if (!result.config.performance().game_mode_valid) {
    result.error = "GameMode policy is invalid";
  } else if (!result.config.performance().physics_worker_mode_valid) {
    result.error = "physics worker policy is invalid";
  } else if (!result.config.fleasion_valid()) {
    result.error = "Fleasion configuration is invalid: use proxy_mode env or hosts, "
                   "a port from 1 to 65535, an absolute CA certificate path, and "
                   "no conflicting system/fixed proxy";
  } else if (!result.config.ca_bundle_valid()) {
    result.error = "CA bundle path is invalid";
  } else if (!result.config.discord_rpc_valid()) {
    result.error = "Discord Rich Presence configuration is invalid";
  }
  return result;
}

bool ExportRuntimeConfigEnvironment(const RuntimeConfig& config,
                                    std::string* error) {
  if (!RuntimeConfig::IsValidPresentMode(config.present_mode())) {
    if (error != nullptr) *error = "cannot export an invalid present mode";
    return false;
  }
  if (!config.fleasion_valid()) {
    if (error != nullptr) *error = "cannot export invalid Fleasion configuration";
    return false;
  }
  if (!config.device_profile_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid device profile";
    }
    return false;
  }
  if (!config.audio_output_device_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid audio output device";
    }
    return false;
  }
  if (!config.audio_input_device_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid audio input device";
    }
    return false;
  }
  if (!config.performance().memory_limit_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid memory-limit policy";
    }
    return false;
  }
  if (!config.performance().game_mode_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid GameMode policy";
    }
    return false;
  }
  if (!config.performance().physics_worker_mode_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid physics worker policy";
    }
    return false;
  }
  if (!config.ca_bundle_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid CA bundle path";
    }
    return false;
  }
  if (!config.discord_rpc_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid Discord Rich Presence policy";
    }
    return false;
  }
  const DeviceProfile& device = config.device_profile();
  const bool base_exported =
      SetEnvironmentValue("MOCKTAIL_HEADLESS", config.headless() ? "1" : "0",
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_PROFILE", std::string(device.name),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_CLASS",
                          std::string(DeviceClassName(device.device_class)),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_PLATFORM_NAME",
                          std::string(device.platform_name), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_NAME",
                          std::string(device.display_name), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_MANUFACTURER",
                          std::string(device.manufacturer), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_MODEL", std::string(device.model),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_BRAND", std::string(device.brand),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_CODE",
                          std::string(device.device_code), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_SKU", std::string(device.device_sku),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_SOC_MODEL",
                          std::string(device.soc_model), error) &&
      SetEnvironmentValue("ROBLOX_LIB_PATH",
                          config.roblox_library_path().string(), error) &&
      SetEnvironmentValue("MOCKTAIL_GRAPHICS_BACKEND",
                          config.graphics_backend_name(), error) &&
      SetEnvironmentValue("MOCKTAIL_THEME", config.theme_mode(), error) &&
      SetEnvironmentValue("MOCKTAIL_OVERLAY_LOCATION",
                          config.overlay().location ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_OVERLAY_LOCATION_OFFSET",
          std::to_string(config.overlay().location_offset), error) &&
      SetEnvironmentValue("MOCKTAIL_OVERLAY_FPS",
                          config.overlay().fps ? "on" : "off", error) &&
      SetEnvironmentValue("MOCKTAIL_OVERLAY_PING",
                          config.overlay().ping ? "on" : "off", error) &&
      SetEnvironmentValue("MOCKTAIL_OVERLAY_POSITION",
                          std::string(OverlayPositionName(
                              config.overlay().position)),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_WIDTH",
                          std::to_string(config.window().width), error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_HEIGHT",
                          std::to_string(config.window().height), error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_TITLE", config.window().title, error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_HIGH_DPI",
                          config.window().high_dpi ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_CLOSE_ON_LEAVE",
                          config.window().close_on_leave ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_EXCLUSIVE_FULLSCREEN",
                          config.window().exclusive_fullscreen ? "1" : "0",
                          error) &&
      SetEnvironmentValue(
          "MOCKTAIL_TOUCH_MODE",
          config.input_capabilities().touch_enabled ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_MOUSE_MODE",
          config.input_capabilities().mouse_enabled ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_KEYBOARD_MODE",
          config.input_capabilities().keyboard_enabled ? "on" : "off", error) &&
      SetEnvironmentValue("MOCKTAIL_RAW_MOUSE",
                          config.input_capabilities().raw_mouse ? "on" : "off",
                          error) &&
      SetEnvironmentValue(
          "MOCKTAIL_SYSTEM_CURSOR",
          config.input_capabilities().system_cursor ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_HARDWARE_CURSOR",
          config.input_capabilities().hardware_cursor ? "on" : "off", error) &&
      SetEnvironmentValue("MOCKTAIL_INPUT_PUMP_HZ",
                          std::to_string(config.input_capabilities().pump_hz),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DESKTOP_PLAYABILITY",
                          config.desktop_playability() ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_FRAME_RATE_LIMIT",
                          FrameRateValue(config.frame_rate()), error) &&
      SetEnvironmentValue("MOCKTAIL_VSYNC", config.vsync_mode(), error) &&
      SetEnvironmentValue("MOCKTAIL_ETC2_BC_TRANSCODE",
                          config.texture_compression() ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_SMALL_TEXTURE_UPSCALE",
                          config.small_texture_upscale(), error) &&
      SetEnvironmentValue("MOCKTAIL_PRESENT_MODE", config.present_mode(),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_PIPELINE_CACHE_MODE",
                          config.pipeline_cache_mode(), error) &&
      SetEnvironmentValue("MOCKTAIL_GRAPHICS_QUALITY",
                          config.graphics_quality().value(), error) &&
      SetEnvironmentValue("MOCKTAIL_TEXTURE_MEMORY_MB",
                          std::to_string(config.texture_memory_mb()),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_AUDIO_BUFFER_SAMPLES",
                          std::to_string(config.audio_buffer_samples()),
                          error) &&
      SetEnvironmentValue(
          "MOCKTAIL_MULTITHREADED_RENDERING",
          config.performance().multithreaded_rendering ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_PHYSICS_WORKER_MODE",
                          std::string(PhysicsWorkerModeName(
                              config.performance().physics_worker_mode)),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_MEMORY_LIMIT_MB",
                          std::to_string(config.performance().memory_limit_mb),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_GAMEMODE",
                          GameModePolicyName(config.performance().game_mode),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_AUDIO_OUTPUT_DEVICE",
                          config.audio_output_device(), error) &&
      SetEnvironmentValue("MOCKTAIL_AUDIO_INPUT_DEVICE",
                          config.audio_input_device(), error) &&
      SetEnvironmentValue("MOCKTAIL_USE_SYSTEM_PROXY",
                          config.use_system_proxy() ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_ENABLED",
                          config.discord_rpc().enabled ? "1" : "0", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME",
          config.discord_rpc().show_place_name ? "1" : "0", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME",
          config.discord_rpc().show_elapsed_time ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_JOIN_ENABLED",
                          config.discord_rpc().join_enabled ? "1" : "0",
                          error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY",
          config.discord_rpc().public_servers_only ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
                          config.discord_rpc().join_button_label, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_APPLICATION_ID",
                          config.discord_rpc().application_id, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
                          config.discord_rpc().text.browsing, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_JOINING",
                          config.discord_rpc().text.joining, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_PLAYING",
                          config.discord_rpc().text.playing, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_STATE",
                          config.discord_rpc().text.state, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
                          config.discord_rpc().text.unknown_place, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_TITLE",
                          config.discord_rpc().text.title, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_IMAGE_LARGE",
                          config.discord_rpc().images.large, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_IMAGE_LARGE_TEXT",
                          config.discord_rpc().images.large_text, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_IMAGE_SMALL",
                          config.discord_rpc().images.small, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_IMAGE_SMALL_TEXT",
                          config.discord_rpc().images.small_text, error);
  if (!base_exported) {
    return false;
  }
  if (config.mangohud() && !SetEnvironmentValue("MANGOHUD", "1", error)) {
    return false;
  }
  if (!SetEnvironmentValue("MOCKTAIL_FLEASION_ENABLED",
                           config.fleasion_enabled() ? "1" : "0", error) ||
      !SetEnvironmentValue("MOCKTAIL_FLEASION_PROXY_MODE",
                           config.fleasion_proxy_mode(), error) ||
      !SetEnvironmentValue("MOCKTAIL_FLEASION_PROXY_PORT",
                           std::to_string(config.fleasion_proxy_port()), error) ||
      !SetEnvironmentValue("MOCKTAIL_FLEASION_CLEAR_CACHE_ON_LAUNCH",
                           config.fleasion_clear_cache_on_launch() ? "1" : "0",
                           error))
    return false;
  if (config.fleasion_ca_certificate()) {
    if (!SetEnvironmentValue("MOCKTAIL_FLEASION_CA_CERTIFICATE",
                             config.fleasion_ca_certificate()->string(), error))
      return false;
  } else if (!UnsetEnvironmentValue("MOCKTAIL_FLEASION_CA_CERTIFICATE", error)) {
    return false;
  }
  if (config.ca_bundle().has_value()) {
    if (!SetEnvironmentValue("MOCKTAIL_CA_BUNDLE",
                             config.ca_bundle()->string(), error)) {
      return false;
    }
  } else if (!UnsetEnvironmentValue("MOCKTAIL_CA_BUNDLE", error)) {
    return false;
  }
  if (config.roblox_http_user_agent().has_value() &&
      !SetEnvironmentValue("MOCKTAIL_USER_AGENT",
                           *config.roblox_http_user_agent(), error)) {
    return false;
  }
  if (!config.roblox_http_user_agent().has_value() &&
      !UnsetEnvironmentValue("MOCKTAIL_USER_AGENT", error)) {
    return false;
  }
  return base_exported && ExportNetworkProxyEnvironment(config, error);
}

bool ExportNetworkProxyEnvironment(const RuntimeConfig& config,
                                   std::string* error) {
  if (!config.network_proxy().has_value()) return true;
  const std::string proxy_url =
      BuildNetworkProxyUrl(*config.network_proxy(), true);
  const bool native_proxy = config.network_proxy()->scheme == "http" &&
                            config.network_proxy()->username.empty();
  return SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_HOST",
                             config.network_proxy()->host, error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_PORT",
                             std::to_string(config.network_proxy()->port),
                             error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_SCHEME",
                             config.network_proxy()->scheme, error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_USERNAME",
                             config.network_proxy()->username, error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_PASSWORD",
                             config.network_proxy()->password, error) &&
         SetEnvironmentValue("http_proxy", proxy_url, error) &&
         SetEnvironmentValue("https_proxy", proxy_url, error) &&
         SetEnvironmentValue("all_proxy", proxy_url, error) &&
         SetEnvironmentValue("no_proxy", "", error) &&
         SetEnvironmentValue("NO_PROXY", "", error) &&
         SetEnvironmentValue("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY",
                             native_proxy ? "1" : "0", error);
}

}  // namespace runtime
}  // namespace mocktail
