#ifndef MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_
#define MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_

#include <curl/curl.h>

#include <optional>
#include <string>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

struct SystemProxyResult {
  std::optional<NetworkProxyConfig> proxy;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Selects the first proxy returned by the host resolver. An empty `proxy`
// means that the resolver selected a direct connection.
SystemProxyResult SelectSystemProxy(
    const std::vector<std::string>& candidates);

// Resolves the host's proxy policy for Roblox HTTPS traffic. GIO delegates to
// the desktop/environment proxy resolver, including PAC when it is available.
// HTTP/HTTPS resolver results and SOCKS5 proxy endpoints are supported.
SystemProxyResult ResolveSystemProxy();

// Applies MOCKTAIL_HTTP_PROXY_HOST / _PORT / _SCHEME, and the optional
// _USERNAME / _PASSWORD login, to a libcurl easy handle, and leaves the handle
// untouched when the environment does not configure one.
//
// This block was copied into every HTTP call site by hand and the copies
// drifted: the server info lookup built "http://host:port" without consulting
// the scheme or the shared parser, so a configured SOCKS5 or HTTPS proxy was
// silently ignored there. One implementation is the fix.
//
// NOPROXY is cleared whenever a proxy is applied, so an inherited no_proxy
// entry (including "*" from a desktop launcher) cannot bypass a proxy the user
// configured on purpose.
CURLcode ApplyConfiguredProxy(CURL* handle);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_
