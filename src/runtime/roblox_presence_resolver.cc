#include "runtime/roblox_presence_resolver.h"

#include <curl/curl.h>


#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "net/curl_support.h"

#include "runtime/environment.h"
#include "runtime/roblox_cookie_store.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

// Matches the ceiling the other Roblox API readers use.
constexpr std::size_t kMaximumResponseBytes = 512U * 1024U;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

}  // namespace

std::string ReadStoredRoblosecurityCookie(
    std::shared_ptr<RobloxSecretStore> secrets) {
  const ProcessEnvironment environment;
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  if (secrets == nullptr) secrets = OpenRobloxSecretStore();
  StoredCookie cookie;
  if (environment.HasNonEmpty("MOCKTAIL_ROBLOX_COOKIES")) {
    cookie.status = StoredCookieStatus::kFound;
    cookie.value = environment.GetOr("MOCKTAIL_ROBLOX_COOKIES", "");
  } else if (environment.HasNonEmpty("MOCKTAIL_COOKIE_FILE")) {
    cookie = ReadRobloxCookieFile(environment.GetOr("MOCKTAIL_COOKIE_FILE", ""),
                                  true);
  } else {
    cookie = LoadManagedRobloxCookie(paths.cookie_file(), *secrets);
  }
  if (cookie.status != StoredCookieStatus::kFound) return {};
  std::string value;
  std::string_view remaining(cookie.value);
  while (!remaining.empty()) {
    const auto end = remaining.find_first_of(";\r\n\t");
    auto segment = remaining.substr(0, end);
    while (!segment.empty() && segment.front() == ' ') segment.remove_prefix(1);
    constexpr std::string_view prefix = ".ROBLOSECURITY=";
    if (segment.substr(0, prefix.size()) == prefix) {
      value = segment.substr(prefix.size());
      break;
    }
    if (segment.substr(0, 2) == "_|") {
      value = segment;
      break;
    }
    if (end == remaining.npos) break;
    remaining.remove_prefix(end + 1);
  }
  volatile char* bytes = cookie.value.empty() ? nullptr : cookie.value.data();
  for (size_t i = 0; i < cookie.value.size(); ++i) bytes[i] = '\0';
  for (unsigned char ch : value) {
    if (ch <= 0x20 || ch >= 0x7f || ch == ';') return {};
  }
  return value;
}

Status ResolveFollowUserPlace(int64_t user_id, const std::string& auth_cookie,
                              ResolvedFollowUserPlace* out) {
  if (out == nullptr || user_id <= 0) {
    return Invalid("Roblox presence lookup requires a positive userId");
  }
  if (auth_cookie.empty()) {
    return Invalid("Roblox presence lookup requires an authenticated session");
  }

  const net::EasyHandle curl(true);
  if (!curl) {
    return Invalid("Roblox presence lookup could not initialize a request");
  }

  const nlohmann::json body = {{"userIds", {user_id}}};
  const std::string payload = body.dump();
  std::string response;

  const std::string cookie_header = "Cookie: .ROBLOSECURITY=" + auth_cookie;
  // Reported rather than ignored, so a failed allocation cannot send the
  // .ROBLOSECURITY cookie request with no cookie attached.
  const net::HeaderList headers(
      std::vector<std::string>{"Content-Type: application/json", cookie_header});
  if (!headers.allocated()) {
    return Invalid("Roblox presence lookup could not build its request");
  }

  curl_easy_setopt(curl.get(), CURLOPT_URL,
                   "https://presence.roblox.com/v1/presence/users");
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  // This writer had no size bound at all, unlike the other Roblox API call
  // sites; the shared sink caps it at the same ceiling they use and checks
  // size * count for overflow.
  net::BoundedStringSink writer{&response, kMaximumResponseBytes, false};
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, net::WriteBoundedString);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &writer);
  net::TransportOptions options;
  options.timeout_ms = 10000L;
  options.follow_redirects = false;
  net::ApplyTransportOptions(curl.get(), options);

  const CURLcode result = curl_easy_perform(curl.get());
  long http_status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);

  if (result != CURLE_OK) {
    return Invalid("Roblox presence lookup failed to connect");
  }
  if (writer.exceeded) {
    return Invalid("Roblox presence lookup response exceeded its size limit");
  }
  if (http_status != 200) {
    return Invalid("Roblox presence lookup was rejected (status " +
                   std::to_string(http_status) + ")");
  }

  const nlohmann::json parsed = nlohmann::json::parse(
      response, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return Invalid("Roblox presence lookup returned malformed JSON");
  }

  if (!parsed.contains("userPresences") ||
      !parsed["userPresences"].is_array() ||
      parsed["userPresences"].empty()) {
    return Invalid("Roblox presence lookup returned no data for user");
  }

  const auto& presence = parsed["userPresences"][0];
  // userPresenceType: 0 offline, 1 online, 2 in-game, 3 in-studio.
  const int presence_type = presence.value("userPresenceType", 0);
  if (presence_type != 2) {
    return Invalid("User is not currently in a joinable Roblox experience");
  }
  if (!presence.contains("placeId") || presence["placeId"].is_null()) {
    return Invalid("Roblox presence lookup did not include a placeId");
  }

  out->place_id = presence.value("placeId", int64_t{0});
  if (presence.contains("gameId") && !presence["gameId"].is_null()) {
    out->game_instance_id = presence.value("gameId", std::string());
  }
  if (out->place_id <= 0) {
    return Invalid("Roblox presence lookup returned an invalid placeId");
  }
  return Status::Ok();
}

}  // namespace runtime
}  // namespace mocktail
