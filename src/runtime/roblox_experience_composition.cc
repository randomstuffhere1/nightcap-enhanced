#include "runtime/roblox_experience_composition.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

#include "jnivm/jnivm.h"
#include "runtime/external_launch_broker.h"
#include "runtime/roblox_launch_uri.h"
#include "runtime/webview_helper_launcher.h"
#include "window/window.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr size_t kMaxPendingLaunchRequests = 8;
// libroblox's static TLS requires the proven 16 MiB guest-thread stack floor.
constexpr size_t kLaunchWorkerStackSize = 64ULL * 1024 * 1024;
// Covers helper exec through window creation, including cold Flatpak starts.
constexpr std::chrono::milliseconds kWebSurfaceReadyTimeout{5000};
// Roblox closes a solved WebViewProtocol challenge itself with closeWindow,
// as on Android. The host closes it only if Roblox has not done so within
// this period. Captcha windows close at once instead (see
// RobloxWebViewOpenRequest::close_when_challenge_solved).
constexpr std::chrono::milliseconds kSolvedChallengeCloseGrace{3000};
constexpr char kRobloxBaseUrl[] = "https://www.roblox.com/";

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

GameSurface SnapshotProductionSurface(void*) {
  const window::WindowSurfaceSnapshot snapshot =
      window::GetWindowSurfaceSnapshot();
  return {snapshot.generation, snapshot.available ? snapshot.native_window : 0,
          snapshot.available ? snapshot.width : 0,
          snapshot.available ? snapshot.height : 0,
          snapshot.available ? snapshot.dpi_scale : 0.0f};
}

Status SurfaceUpdateStatus(const GameSessionUpdateResult& result) {
  if (result.ok()) {
    return Status::Ok();
  }
  return result.cause.ok() ? FailedPrecondition(result.message) : result.cause;
}

bool SameSurface(const GameSurface& left, const GameSurface& right) {
  return left.generation == right.generation &&
         left.native_window == right.native_window &&
         left.width == right.width && left.height == right.height &&
         left.dpi_scale == right.dpi_scale;
}

int32_t JoinRequestTypeFor(const RobloxExperienceLaunchRequest& request) {
  // Mirrors APK zh.l0.a/b. Empty JSON strings are equivalent to the APK's
  // TextUtils.isEmpty -> null normalization.
  if (request.conversation_id > 0) {
    return 6;
  }
  if (request.user_id > 0) {
    return 1;
  }
  if (!request.access_code.empty() || !request.link_code.empty()) {
    return 2;
  }
  if (!request.game_instance_id.empty()) {
    return 3;
  }
  if (!request.reserved_server_access_code.empty()) {
    return 8;
  }
  return 0;
}

GameSessionUpdateResult AcceptedLuaAppSurface(std::string message) {
  return {GameSessionUpdateStatus::kApplied, GameSessionState::kCreated,
          Status::Ok(), std::move(message)};
}

GameSessionUpdateResult RejectedLuaAppSurface(Status status) {
  const std::string message = status.message();
  return {GameSessionUpdateStatus::kRejected, GameSessionState::kFailure,
          std::move(status), message};
}

Status CheckJni(JNIEnv* env, const char* operation) {
  if (env == nullptr) {
    return Unavailable("JNIEnv is unavailable in experience composition");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return Status::Error(
      StatusCode::kPlatformError,
      std::string(operation != nullptr ? operation : "JNI operation") +
          " failed in experience composition");
}

Status PublishSystemTheme(JNIEnv* env,
                          const RobloxWebViewMessageBusSymbols& symbols,
                          jobject message_bus) {
  jclass message_bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jstring protocol = env->NewStringUTF("SystemTheme");
  jstring message = env->NewStringUTF("systemThemeUpdated");
  jstring payload = env->NewStringUTF("{}");
  jstring message_id = nullptr;
  Status status = Status::Ok();
  if (message_bus_class == nullptr || protocol == nullptr ||
      message == nullptr || payload == nullptr) {
    status = Unavailable("could not allocate SystemTheme MessageBus values");
  } else {
    message_id =
        symbols.get_message_id(env, message_bus_class, protocol, message);
    status = CheckJni(env, "compose SystemTheme.systemThemeUpdated id");
  }
  if (status.ok() && message_id == nullptr) {
    status = Unavailable("SystemTheme.systemThemeUpdated id is unavailable");
  }
  if (status.ok()) {
    symbols.publish_raw(env, message_bus, message_id, payload);
    status = CheckJni(env, "publish SystemTheme.systemThemeUpdated");
  }

  if (message_id != nullptr)
    env->DeleteLocalRef(message_id);
  if (payload != nullptr)
    env->DeleteLocalRef(payload);
  if (message != nullptr)
    env->DeleteLocalRef(message);
  if (protocol != nullptr)
    env->DeleteLocalRef(protocol);
  if (message_bus_class != nullptr)
    env->DeleteLocalRef(message_bus_class);
  return status;
}

Status LaunchRobloxWebSurface(
    const std::string& url, const char* transport,
    WebViewHelperExitObserver exit_observer = {},
    std::shared_ptr<WebViewHelperProcess>* process = nullptr) {
  std::filesystem::path helper;
  const char* helper_override = std::getenv("MOCKTAIL_WEBVIEW_HELPER");
  if (helper_override != nullptr && helper_override[0] != '\0') {
    helper = helper_override;
  } else {
    std::error_code error;
    const std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || executable.empty()) {
      return Unavailable("could not resolve Mocktail executable directory");
    }
    helper = executable.parent_path() / "mocktail_webview_helper";
  }
  std::fprintf(stderr, "  [%s] opening validated Roblox web surface\n",
               transport);
  const auto started = std::chrono::steady_clock::now();
  const WebViewHelperLaunchResult launched =
      LaunchWebViewHelper(helper, url, std::move(exit_observer));
  if (!launched) {
    return Unavailable("could not display Roblox web surface: " +
                       launched.error);
  }
  const bool ready = launched.process != nullptr &&
                     launched.process->WaitUntilReady(kWebSurfaceReadyTimeout);
  const long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  if (!ready) {
    if (launched.process != nullptr) {
      (void)launched.process->RequestClose();
    }
    return Unavailable(
        "Roblox web surface did not become ready before the deadline (" +
        std::to_string(elapsed_ms) + " ms of " +
        std::to_string(kWebSurfaceReadyTimeout.count()) + " ms)");
  }
  if (process != nullptr) {
    *process = launched.process;
  }
  std::fprintf(stderr, "  [%s] Roblox web surface launched in %lld ms\n",
               transport, elapsed_ms);
  return Status::Ok();
}

template <typename Value>
bool SetField(JNIEnv* env, jobject object, jclass clazz, const char* name,
              const char* signature, Value value);

template <>
bool SetField<jint>(JNIEnv* env, jobject object, jclass clazz, const char* name,
                    const char* signature, jint value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr)
    return false;
  env->SetIntField(object, field, value);
  return true;
}

template <>
bool SetField<jlong>(JNIEnv* env, jobject object, jclass clazz,
                     const char* name, const char* signature, jlong value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr)
    return false;
  env->SetLongField(object, field, value);
  return true;
}

template <>
bool SetField<jboolean>(JNIEnv* env, jobject object, jclass clazz,
                        const char* name, const char* signature,
                        jboolean value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr)
    return false;
  env->SetBooleanField(object, field, value);
  return true;
}

template <>
bool SetField<jfloat>(JNIEnv* env, jobject object, jclass clazz,
                      const char* name, const char* signature, jfloat value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr)
    return false;
  env->SetFloatField(object, field, value);
  return true;
}

template <>
bool SetField<jobject>(JNIEnv* env, jobject object, jclass clazz,
                       const char* name, const char* signature, jobject value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr)
    return false;
  env->SetObjectField(object, field, value);
  return true;
}

Status Allocate(JNIEnv* env, const char* class_name, jclass* clazz,
                jobject* object) {
  *clazz = env->FindClass(class_name);
  if (*clazz == nullptr) {
    return Unavailable(std::string("required JNI class is unavailable: ") +
                       class_name);
  }
  *object = env->AllocObject(*clazz);
  if (*object == nullptr) {
    env->DeleteLocalRef(*clazz);
    *clazz = nullptr;
    return Unavailable(std::string("could not allocate JNI class: ") +
                       class_name);
  }
  return CheckJni(env, class_name);
}

}  // namespace

struct RobloxExperienceComposition::GlobalObjects {
  jclass native_gl_class = nullptr;
  jobject activity = nullptr;
  jobject message_bus = nullptr;
};

struct RobloxExperienceComposition::LaunchTask {
  RobloxExperienceLaunchRequest request;
  RobloxFreshGameLaunchContext context;
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  Status result = Status::Ok();
};

struct RobloxExperienceComposition::WebSurfaceExitTarget {
  std::mutex mutex;
  RobloxExperienceComposition* composition = nullptr;
};

struct RobloxExperienceComposition::WebSurfaceExitContext {
  std::shared_ptr<WebSurfaceExitTarget> target;
  uint64_t process_generation = 0;
};

struct RobloxExperienceComposition::LifecycleTarget {
  std::mutex mutex;
  RobloxExperienceComposition* composition = nullptr;
};

bool RobloxLuaAppExperienceReadiness::complete() const {
  const bool principal_valid =
      principal.kind == GameSessionPrincipalKind::kAuthenticated &&
      principal.generation != 0 && !principal.principal_id.empty() &&
      !principal.base_url.empty();
  return principal_valid && surface.generation != 0 &&
         surface.native_window != 0 && surface.width != 0 &&
         surface.height != 0 && !username.empty();
}

RobloxExperienceComposition::RobloxExperienceComposition(
    JniEnvironmentProvider environment,
    RobloxExperienceMessageBusSymbols message_bus_symbols,
    RobloxWebViewMessageBusSymbols web_view_symbols,
    RobloxBrowserServiceSymbols browser_service_symbols,
    RobloxPermissionsMessageBusSymbols permissions_symbols,
    RobloxGameSessionSymbols game_symbols,
    RobloxExperienceJniFactory jni_factory,
    RobloxFreshLaunchPresentBoundary present_boundary,
    RobloxGameSurfaceJniConfig surface_config,
    const SecureRobloxCredential* initial_web_view_credential,
    RobloxExperienceSurfaceProvider surface_provider,
    RobloxExperiencePresenceObserver presence_observer,
    bool clear_persisted_web_view_cookie, bool microphone_enabled,
    RobloxAccountProtocolSymbols account_symbols)
    : environment_(environment),
      message_bus_symbols_(message_bus_symbols),
      web_view_symbols_(web_view_symbols),
      browser_service_symbols_(browser_service_symbols),
      permissions_symbols_(permissions_symbols),
      account_symbols_(account_symbols),
      microphone_enabled_(microphone_enabled),
      game_symbols_(game_symbols),
      jni_factory_(jni_factory),
      present_boundary_(present_boundary),
      surface_config_(std::move(surface_config)),
      consumes_window_surface_events_(!surface_provider.valid()),
      surface_provider_(
          surface_provider.valid()
              ? surface_provider
              : RobloxExperienceSurfaceProvider{nullptr,
                                                &SnapshotProductionSurface}),
      presence_observer_(presence_observer),
      web_surface_exit_target_(std::make_shared<WebSurfaceExitTarget>()),
      lifecycle_target_(std::make_shared<LifecycleTarget>()),
      clear_persisted_web_view_cookie_(clear_persisted_web_view_cookie) {
  web_surface_exit_target_->composition = this;
  lifecycle_target_->composition = this;
  if (initial_web_view_credential != nullptr) {
    WebViewRobloxCookieResult prepared =
        PrepareWebViewRobloxCookie(*initial_web_view_credential);
    if (prepared) {
      web_view_cookie_ = std::move(prepared.cookie);
      web_view_cookie_synchronized_ = true;
    } else {
      web_view_cookie_initialization_error_ = std::move(prepared.error);
    }
  }
}

RobloxExperienceComposition::~RobloxExperienceComposition() {
  Shutdown();
}

Status RobloxExperienceComposition::InitializePlatformProtocols() {
  if (!web_view_cookie_initialization_error_.empty()) {
    return FailedPrecondition(web_view_cookie_initialization_error_);
  }
  if (!environment_.valid() || !web_view_symbols_.complete() ||
      !browser_service_symbols_.complete() ||
      !permissions_symbols_.complete() || !jni_factory_.complete()) {
    return FailedPrecondition("platform protocol prerequisites are incomplete");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (platform_protocols_initialized_ || objects_ != nullptr ||
        web_view_bridge_ != nullptr || permissions_bridge_ != nullptr ||
        call_protocol_bridge_ != nullptr) {
      return FailedPrecondition("platform protocols are already initialized");
    }
  }

  Status status = BuildPlatformGlobalObjects();
  if (!status.ok()) {
    (void)ReleaseGlobalObjects();
    return status;
  }

  RobloxWebViewMessageBusObjects web_view_objects;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    web_view_objects.message_bus =
        objects_ != nullptr ? objects_->message_bus : nullptr;
  }
  if (web_view_objects.message_bus == nullptr) {
    (void)ReleaseGlobalObjects();
    return FailedPrecondition("platform MessageBus root is unavailable");
  }
  web_view_objects.callback_factory_context = jni_factory_.context;
  web_view_objects.create_raw_callback = jni_factory_.create_raw_callback;
  web_view_objects.clear_raw_callback = jni_factory_.clear_raw_callback;
  web_view_objects.create_request_handler = jni_factory_.create_request_handler;
  web_view_objects.clear_request_handler = jni_factory_.clear_request_handler;
  web_view_objects.set_platform_web_callbacks =
      jni_factory_.set_platform_web_callbacks;
  web_view_objects.clear_platform_web_callbacks =
      jni_factory_.clear_platform_web_callbacks;

  auto web_view_bridge = std::make_unique<RobloxWebViewBridge>(
      environment_, web_view_symbols_, web_view_objects,
      RobloxWebViewSink{this, &RobloxExperienceComposition::DispatchWebViewOpen,
                        &RobloxExperienceComposition::DispatchWebViewMutate,
                        &RobloxExperienceComposition::DispatchWebViewClose,
                        &RobloxExperienceComposition::DispatchWebViewCookie});
  status = web_view_bridge->Initialize();
  if (!status.ok()) {
    (void)ReleaseGlobalObjects();
    return status;
  }

  RobloxBrowserServiceCallbackFactory factory;
  factory.context = jni_factory_.context;
  factory.create_callback = jni_factory_.create_mem_storage_callback;
  factory.clear_callback = jni_factory_.clear_mem_storage_callback;
  auto browser_service_bridge = std::make_unique<RobloxBrowserServiceBridge>(
      environment_, browser_service_symbols_, factory,
      RobloxBrowserServiceSink{
          this, &RobloxExperienceComposition::DispatchBrowserServiceOpen,
          &RobloxExperienceComposition::DispatchBrowserServiceClose,
          &RobloxExperienceComposition::DispatchBrowserServiceConfig,
          &RobloxExperienceComposition::DispatchBrowserServiceExecute});
  status = browser_service_bridge->Initialize();
  if (!status.ok()) {
    (void)web_view_bridge->Shutdown();
    (void)ReleaseGlobalObjects();
    return status;
  }
  RobloxPermissionsMessageBusObjects permissions_objects{
      web_view_objects.message_bus,
      jni_factory_.context,
      jni_factory_.create_raw_callback,
      jni_factory_.clear_raw_callback,
      jni_factory_.create_async_request_handler,
      jni_factory_.clear_async_request_handler};
  auto permissions_bridge = std::make_unique<RobloxPermissionsBridge>(
      environment_, permissions_symbols_, permissions_objects,
      microphone_enabled_);
  status = permissions_bridge->Initialize();
  if (!status.ok()) {
    (void)browser_service_bridge->Shutdown();
    (void)web_view_bridge->Shutdown();
    (void)ReleaseGlobalObjects();
    return status;
  }
  auto call_protocol_bridge = std::make_unique<RobloxCallProtocolBridge>(
      environment_, permissions_symbols_, permissions_objects);
  status = call_protocol_bridge->Initialize();
  if (!status.ok()) {
    (void)permissions_bridge->Shutdown();
    (void)browser_service_bridge->Shutdown();
    (void)web_view_bridge->Shutdown();
    (void)ReleaseGlobalObjects();
    return status;
  }
  // AccountProtocol only shortens a device-integrity login challenge that
  // cannot succeed here, so its absence or failure never blocks startup.
  std::unique_ptr<RobloxAccountBridge> account_bridge;
  if (account_symbols_.complete()) {
    account_bridge = std::make_unique<RobloxAccountBridge>(
        environment_, account_symbols_,
        RobloxAccountProtocolObjects{web_view_objects.message_bus,
                                     jni_factory_.context,
                                     jni_factory_.create_request_handler,
                                     jni_factory_.clear_request_handler});
    const Status account_status = account_bridge->Initialize();
    if (!account_status.ok()) {
      std::fprintf(stderr, "  [account] AccountProtocol unavailable: %s\n",
                   account_status.message().c_str());
      account_bridge.reset();
    }
  } else {
    std::fprintf(stderr,
                 "  [account] AccountProtocol entrypoints are missing; "
                 "device-integrity challenges wait for Roblox's timeout\n");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    web_view_bridge_ = std::move(web_view_bridge);
    browser_service_bridge_ = std::move(browser_service_bridge);
    permissions_bridge_ = std::move(permissions_bridge);
    call_protocol_bridge_ = std::move(call_protocol_bridge);
    account_bridge_ = std::move(account_bridge);
    platform_protocols_initialized_ = true;
  }
  std::fprintf(stderr,
               "  [platform] Android WebViewProtocol, BrowserService and "
               "PermissionsProtocol and CallProtocol "
               "bridges ready\n");
  return Status::Ok();
}

Status RobloxExperienceComposition::OnLuaAppReady(
    RobloxLuaAppExperienceReadiness readiness) {
  if (!readiness.complete()) {
    return Invalid("LuaApp experience readiness is incomplete");
  }
  if (!environment_.valid() || !message_bus_symbols_.complete() ||
      !game_symbols_.complete() || !jni_factory_.complete() ||
      !present_boundary_.complete() || !surface_config_.complete()) {
    return FailedPrecondition(
        "experience composition prerequisites are incomplete");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!platform_protocols_initialized_ || objects_ == nullptr ||
        objects_->message_bus == nullptr || web_view_bridge_ == nullptr ||
        browser_service_bridge_ == nullptr || permissions_bridge_ == nullptr ||
        call_protocol_bridge_ == nullptr) {
      return FailedPrecondition(
          "platform protocols must be initialized before LuaApp readiness");
    }
    if (objects_->native_gl_class != nullptr || objects_->activity != nullptr ||
        bridge_ != nullptr || controller_ != nullptr) {
      return FailedPrecondition(
          "LuaApp experience composition is already ready");
    }
  }
  Status status = BuildLuaAppGlobalObjects();
  if (!status.ok()) {
    (void)ReleaseLuaAppGlobalObjects();
    return status;
  }

  auto controller = std::make_shared<RobloxFreshGameLaunchController>(
      environment_, game_symbols_, present_boundary_, surface_config_,
      RobloxGamePresentedObserver{this,
                                  &RobloxExperienceComposition::GamePresented});
  RobloxExperienceMessageBusObjects bridge_objects;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    readiness_ = std::move(readiness);
    controlled_switch_waiting_for_return_ = false;
    lua_app_surface_recreation_pending_ = false;
    controller_ = controller;
    bridge_objects.message_bus = objects_->message_bus;
    subscribed_ = true;
  }
  bridge_objects.callback_factory_context = jni_factory_.context;
  bridge_objects.create_raw_callback = jni_factory_.create_raw_callback;
  bridge_objects.clear_raw_callback = jni_factory_.clear_raw_callback;
  auto bridge = std::make_unique<RobloxExperienceLaunchBridge>(
      environment_, message_bus_symbols_, bridge_objects,
      RobloxExperienceLaunchSink{this,
                                 &RobloxExperienceComposition::DispatchLaunch});
  status = bridge->Initialize();
  if (status.ok()) {
    JNIEnv* env = nullptr;
    status = environment_.Acquire(&env);
    if (status.ok()) {
      status = PublishSystemTheme(env, web_view_symbols_,
                                  bridge_objects.message_bus);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status.ok()) {
      bridge_ = std::move(bridge);
    } else {
      subscribed_ = false;
      controller_.reset();
    }
  }
  if (!status.ok())
    (void)ReleaseLuaAppGlobalObjects();
  if (status.ok()) {
    std::fprintf(stderr,
                 "  [theme] published SystemTheme.systemThemeUpdated (%s)\n",
                 std::getenv("MOCKTAIL_RESOLVED_THEME_INTERNAL") != nullptr
                     ? std::getenv("MOCKTAIL_RESOLVED_THEME_INTERNAL")
                     : "Dark");
  }
  return status.ok() ? DrainExternalLaunchRequests() : status;
}

Status RobloxExperienceComposition::OpenWebSurface(
    const std::string& url, const char* transport, WebSurfaceRoute route,
    WebViewHelperExitObserver exit_observer,
    const WebSurfacePresentation& presentation) {
  if (route == WebSurfaceRoute::kNone || !exit_observer.valid()) {
    return Invalid("WebView surface open request is incomplete");
  }
  std::lock_guard<std::mutex> operation_lock(web_surface_operation_mutex_);

  std::shared_ptr<WebViewHelperProcess> current;
  SecureWebViewRobloxCookie cookie;
  uint64_t process_generation = 0;
  uint64_t logical_generation = 0;
  bool clear_persisted_cookie = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!web_view_cookie_synchronized_) {
      return FailedPrecondition("WebView authentication is not synchronized");
    }
    current = web_surface_process_;
    cookie = web_view_cookie_.Clone();
    clear_persisted_cookie = clear_persisted_web_view_cookie_;
    process_generation = web_surface_process_generation_;
    logical_generation = next_web_surface_logical_generation_++;
    if (logical_generation == 0 || next_web_surface_logical_generation_ == 0) {
      return FailedPrecondition(
          "WebView logical surface generation is exhausted");
    }
  }

  const bool spawn = current == nullptr || !current->running();
  if (spawn) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      process_generation = next_web_surface_process_generation_++;
      if (process_generation == 0 ||
          next_web_surface_process_generation_ == 0) {
        return FailedPrecondition("WebView process generation is exhausted");
      }
    }
    auto exit_context = std::make_shared<WebSurfaceExitContext>();
    exit_context->target = web_surface_exit_target_;
    exit_context->process_generation = process_generation;
    Status status = LaunchRobloxWebSurface(
        url, transport,
        WebViewHelperExitObserver{
            exit_context, &RobloxExperienceComposition::WebSurfaceExited},
        &current);
    if (!status.ok()) {
      return status;
    }
  }

  // Serialize setup so presentation state cannot leak between routes.
  const bool cookie_synchronized =
      clear_persisted_cookie ? current->ClearRobloxCookie()
                             : current->SetRobloxCookie(cookie.value());
  // Navigate while the previous surface is still hidden, before the window
  // is presented: a reopened surface must render the new load, never the
  // previous document. The helper replaces its stale WebView before loading,
  // so the title and visibility applied afterwards land on the reset
  // surface.
  const bool navigated = spawn || current->LoadUrl(url);
  if (!current->SetBackNavigationDisabled(
          presentation.back_navigation_disabled) ||
      !current->SetShowDomainAsTitle(presentation.show_domain_as_title) ||
      !cookie_synchronized || !navigated ||
      !current->SetTitle(presentation.title) ||
      !current->SetVisible(presentation.visible)) {
    (void)current->RequestClose();
    return Unavailable("could not configure reusable Roblox web surface");
  }
  if (!current->running()) {
    if (spawn) {
      (void)current->RequestClose();
    }
    return Unavailable("Roblox web surface exited during configuration");
  }

  WebViewHelperExitObserver superseded_observer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    superseded_observer = std::move(web_surface_logical_exit_observer_);
    web_surface_process_ = current;
    web_surface_process_generation_ = process_generation;
    web_surface_route_ = route;
    web_surface_logical_generation_ = logical_generation;
    web_surface_close_when_challenge_solved_ =
        presentation.close_when_challenge_solved;
    web_surface_logical_exit_observer_ = std::move(exit_observer);
  }
  if (superseded_observer.valid()) {
    superseded_observer.on_exit(superseded_observer.context.get());
  }
  if (!current->running()) {
    HandleWebSurfaceExit(process_generation);
    return Unavailable("Roblox web surface exited while committing route");
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::CloseWebSurface(
    uint64_t expected_logical_generation) {
  std::lock_guard<std::mutex> operation_lock(web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  WebViewHelperExitObserver exit_observer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (web_surface_route_ == WebSurfaceRoute::kNone ||
        (expected_logical_generation != 0 &&
         expected_logical_generation != web_surface_logical_generation_)) {
      return Status::Ok();
    }
    process = std::move(web_surface_process_);
    web_surface_process_generation_ = 0;
    exit_observer = std::move(web_surface_logical_exit_observer_);
    web_surface_route_ = WebSurfaceRoute::kNone;
    web_surface_logical_generation_ = 0;
    web_surface_close_when_challenge_solved_ = false;
  }
  // A closed challenge must stop executing. Reusing its hidden document lets
  // late callbacks from the old attempt reach the next login attempt. The APK
  // removes its WebView fragment here and creates a new one on the next open.
  const bool closed =
      process == nullptr || !process->running() || process->RequestClose();
  if (exit_observer.valid()) {
    exit_observer.on_exit(exit_observer.context.get());
  }
  if (!closed) {
    return Unavailable("could not close Roblox web surface");
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::CloseUnclaimedSolvedChallenge() {
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!solved_challenge_close_deadline_.has_value()) {
      return Status::Ok();
    }
    const bool still_open =
        web_surface_route_ != WebSurfaceRoute::kNone &&
        solved_challenge_logical_generation_ == web_surface_logical_generation_;
    if (still_open &&
        std::chrono::steady_clock::now() < *solved_challenge_close_deadline_) {
      return Status::Ok();
    }
    solved_challenge_close_deadline_.reset();
    if (!still_open) {
      return Status::Ok();
    }
    generation = solved_challenge_logical_generation_;
  }
  std::fprintf(stderr,
               "  [webview] closing solved challenge window; Roblox did not "
               "close it within %lld ms\n",
               static_cast<long long>(kSolvedChallengeCloseGrace.count()));
  return CloseWebSurface(generation);
}

void RobloxExperienceComposition::WebSurfaceExited(void* context) {
  auto* exit_context = static_cast<WebSurfaceExitContext*>(context);
  if (exit_context == nullptr || exit_context->target == nullptr ||
      exit_context->process_generation == 0) {
    return;
  }
  std::lock_guard<std::mutex> target_lock(exit_context->target->mutex);
  if (exit_context->target->composition != nullptr) {
    exit_context->target->composition->HandleWebSurfaceExit(
        exit_context->process_generation);
  }
}

void RobloxExperienceComposition::HandleWebSurfaceExit(
    uint64_t process_generation) {
  WebViewHelperExitObserver exit_observer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (web_surface_process_generation_ != process_generation) {
      return;
    }
    web_surface_process_.reset();
    web_surface_process_generation_ = 0;
    web_surface_route_ = WebSurfaceRoute::kNone;
    web_surface_logical_generation_ = 0;
    web_surface_close_when_challenge_solved_ = false;
    exit_observer = std::move(web_surface_logical_exit_observer_);
  }
  if (exit_observer.valid()) {
    exit_observer.on_exit(exit_observer.context.get());
  }
}

Status RobloxExperienceComposition::DispatchWebViewOpen(
    void* context, const RobloxWebViewOpenRequest& request,
    WebViewHelperExitObserver exit_observer) {
  if (context == nullptr) {
    return Invalid("WebView dispatch context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  WebSurfacePresentation presentation;
  if (!request.title.empty()) {
    presentation.title = request.title;
  }
  presentation.visible = request.is_visible.value_or(true);
  presentation.show_domain_as_title =
      request.show_domain_as_title.value_or(false);
  // The host helper has no toolbar, so a hidden Roblox back button disables
  // host back navigation too.
  presentation.back_navigation_disabled =
      request.back_button_visible.has_value() && !*request.back_button_visible;
  presentation.close_when_challenge_solved =
      request.close_when_challenge_solved;
  return composition->OpenWebSurface(request.url, "webview",
                                     WebSurfaceRoute::kWebView,
                                     std::move(exit_observer), presentation);
}

Status RobloxExperienceComposition::DispatchWebViewMutate(
    void* context, const RobloxWebViewMutationRequest& request) {
  if (context == nullptr) {
    return Invalid("WebView mutation context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  std::lock_guard<std::mutex> operation_lock(
      composition->web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    process = composition->web_surface_route_ != WebSurfaceRoute::kNone
                  ? composition->web_surface_process_
                  : nullptr;
  }
  if (process == nullptr || !process->running()) {
    return FailedPrecondition("WebView window is not running");
  }
  if (request.url.has_value() && !process->LoadUrl(*request.url)) {
    return Unavailable("could not update the WebView window URL");
  }
  if (request.title.has_value() && !process->SetTitle(*request.title)) {
    return Unavailable("could not update the WebView window title");
  }
  if (request.is_visible.has_value() &&
      !process->SetVisible(*request.is_visible)) {
    return Unavailable("could not update WebView window visibility");
  }
  if (request.show_domain_as_title.has_value() &&
      !process->SetShowDomainAsTitle(*request.show_domain_as_title)) {
    return Unavailable("could not update WebView domain-title policy");
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::DispatchWebViewClose(void* context) {
  if (context == nullptr) {
    return Invalid("WebView close context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  return composition->CloseWebSurface();
}

Status RobloxExperienceComposition::DispatchWebViewCookie(
    void* context, std::string_view canonical_header) {
  if (context == nullptr) {
    return Invalid("WebView cookie dispatch context is null");
  }
  SecureRobloxCredential credential{std::string(canonical_header)};
  WebViewRobloxCookieResult prepared = PrepareWebViewRobloxCookie(credential);
  if (!prepared) {
    return Invalid(prepared.error);
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  std::lock_guard<std::mutex> operation_lock(
      composition->web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  SecureWebViewRobloxCookie cookie;
  {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    composition->web_view_cookie_ = std::move(prepared.cookie);
    composition->web_view_cookie_synchronized_ = true;
    composition->clear_persisted_web_view_cookie_ = false;
    cookie = composition->web_view_cookie_.Clone();
    process = composition->web_surface_process_;
  }
  if (process != nullptr && process->running() &&
      !process->SetRobloxCookie(cookie.value())) {
    return Unavailable("could not update WebView authentication");
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::AcceptWebViewRobloxCookie(
    std::string_view value) {
  std::string canonical_header = ".ROBLOSECURITY=";
  canonical_header.append(value);
  SecureRobloxCredential credential{std::move(canonical_header)};
  WebViewRobloxCookieResult prepared = PrepareWebViewRobloxCookie(credential);
  if (!prepared) {
    return Invalid(prepared.error);
  }

  jnivm::VM* vm = jnivm::VM::FromJavaVM(environment_.java_vm);
  if (vm == nullptr) {
    return FailedPrecondition("WebView login has no active VM");
  }
  const bool was_guest = vm->GetRobloxAuthIdentitySnapshot().user_id <= 0;
  if (!vm->DispatchRobloxCredential(credential.c_str(), credential.size())) {
    return Unavailable("could not store WebView login credential");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    web_view_cookie_ = std::move(prepared.cookie);
    web_view_cookie_synchronized_ = true;
    clear_persisted_web_view_cookie_ = false;
  }

  if (was_guest && vm->GetRobloxAuthIdentitySnapshot().user_id > 0) {
    const Status close_status = CloseWebSurface();
    if (!close_status.ok()) {
      std::fprintf(stderr,
                   "  [auth] browser sign-in accepted; WebView close failed: "
                   "%s\n",
                   close_status.message().c_str());
    } else {
      std::fprintf(stderr,
                   "  [auth] browser sign-in accepted into the running VM\n");
    }
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::DispatchBrowserServiceOpen(
    void* context, const RobloxBrowserServiceOpenRequest& request,
    WebViewHelperExitObserver exit_observer) {
  if (context == nullptr) {
    return Invalid("BrowserService dispatch context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  WebSurfacePresentation presentation;
  presentation.title = request.title.value_or("Roblox");
  presentation.visible = request.visible.value_or(true);
  presentation.back_navigation_disabled =
      request.back_navigation_disabled.value_or(false);
  presentation.show_domain_as_title =
      request.show_domain_as_title.value_or(false);
  return composition->OpenWebSurface(request.url, "browser",
                                     WebSurfaceRoute::kBrowserService,
                                     std::move(exit_observer), presentation);
}

Status RobloxExperienceComposition::DispatchBrowserServiceClose(void* context) {
  if (context == nullptr) {
    return Invalid("BrowserService close context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  return composition->CloseWebSurface();
}

Status RobloxExperienceComposition::DispatchBrowserServiceConfig(
    void* context, const RobloxBrowserServiceConfigRequest& request) {
  if (context == nullptr) {
    return Invalid("BrowserService config context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  std::lock_guard<std::mutex> operation_lock(
      composition->web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    process = composition->web_surface_route_ != WebSurfaceRoute::kNone
                  ? composition->web_surface_process_
                  : nullptr;
  }
  if (process == nullptr || !process->running()) {
    return FailedPrecondition("BrowserService window is not running");
  }
  if (request.title.has_value() && !process->SetTitle(*request.title)) {
    return Unavailable("could not update BrowserService title");
  }
  if (request.visible.has_value() && !process->SetVisible(*request.visible)) {
    return Unavailable("could not update BrowserService visibility");
  }
  if (request.back_navigation_disabled.has_value() &&
      !process->SetBackNavigationDisabled(*request.back_navigation_disabled)) {
    return Unavailable("could not update BrowserService back navigation");
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::DispatchBrowserServiceExecute(
    void* context, const RobloxBrowserServiceExecuteRequest& request) {
  if (context == nullptr) {
    return Invalid("BrowserService execute context is null");
  }
  auto* composition = static_cast<RobloxExperienceComposition*>(context);
  std::lock_guard<std::mutex> operation_lock(
      composition->web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    process = composition->web_surface_route_ != WebSurfaceRoute::kNone
                  ? composition->web_surface_process_
                  : nullptr;
  }
  if (process == nullptr || !process->running()) {
    return FailedPrecondition("BrowserService window is not running");
  }
  return process->EvaluateJavaScript(request.source)
             ? Status::Ok()
             : Unavailable("could not execute BrowserService JavaScript");
}

Status RobloxExperienceComposition::DispatchLaunch(
    void* context, const RobloxExperienceLaunchRequest& request) {
  if (context == nullptr) {
    return Invalid("experience composition dispatch context is null");
  }
  return static_cast<RobloxExperienceComposition*>(context)->Dispatch(request);
}

Status RobloxExperienceComposition::RouteWebSurfaceEvent(
    WebSurfaceRoute route, const WebViewHelperEvent& event,
    RobloxWebViewBridge* web_view_bridge) {
  switch (event.type) {
    case WebViewHelperEventType::kExecuteRoblox:
      // WebViewProtocol listens on the shared gi.a fragment. Preserve the raw
      // command, including RequestGameJob instance and attempt IDs.
      if (route == WebSurfaceRoute::kNone) {
        return FailedPrecondition(
            "WebView helper event has no active logical route");
      }
      return web_view_bridge != nullptr
                 ? web_view_bridge->SignalJavascriptCallback(event.payload)
                 : FailedPrecondition("WebViewProtocol bridge is unavailable");
    case WebViewHelperEventType::kRobloxWkHybrid:
      if (route == WebSurfaceRoute::kNone) {
        return FailedPrecondition(
            "WebView helper event has no active logical route");
      }
      return web_view_bridge != nullptr
                 ? web_view_bridge->SignalJavascriptCallback(event.payload)
                 : FailedPrecondition("WebViewProtocol bridge is unavailable");
    case WebViewHelperEventType::kReady:
      return Status::Ok();
    case WebViewHelperEventType::kRobloxCookie:
      return FailedPrecondition(
          "WebView cookie event requires the active composition");
  }
  return Status::Error(StatusCode::kUnsupported,
                       "WebView helper event type is unsupported");
}

Status RobloxExperienceComposition::HandleSolvedChallenge(
    uint64_t logical_generation) {
  std::fprintf(stderr, "  [webview] solved challenge forwarded to Roblox\n");
  bool close_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logical_generation != web_surface_logical_generation_) {
      return Status::Ok();
    }
    close_now = web_surface_close_when_challenge_solved_;
    if (!close_now) {
      solved_challenge_close_deadline_ =
          std::chrono::steady_clock::now() + kSolvedChallengeCloseGrace;
      solved_challenge_logical_generation_ = logical_generation;
    }
  }
  if (!close_now) {
    return Status::Ok();
  }
  std::fprintf(stderr, "  [webview] closing solved captcha window\n");
  return CloseWebSurface(logical_generation);
}

Status RobloxExperienceComposition::RouteCurrentWebSurfaceEvent(
    const std::shared_ptr<WebViewHelperProcess>& source_process,
    uint64_t process_generation, uint64_t logical_generation,
    const WebViewHelperEvent& event) {
  std::vector<WebViewHelperEvent> events;
  events.push_back(event);
  return RouteWebSurfaceEventBatch(source_process, process_generation,
                                   logical_generation, events);
}

Status RobloxExperienceComposition::RouteWebSurfaceEventBatch(
    const std::shared_ptr<WebViewHelperProcess>& source_process,
    uint64_t process_generation, uint64_t logical_generation,
    std::vector<WebViewHelperEvent>& events) {
  WebSurfaceRoute route = WebSurfaceRoute::kNone;
  RobloxWebViewBridge* web_view_bridge = nullptr;
  RobloxBrowserServiceBridge* browser_service_bridge = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_process != web_surface_process_ || process_generation == 0 ||
        process_generation != web_surface_process_generation_ ||
        logical_generation == 0 ||
        logical_generation != web_surface_logical_generation_ ||
        web_surface_route_ == WebSurfaceRoute::kNone) {
      // Closing or replacing the helper invalidates buffered surface events.
      for (WebViewHelperEvent& remaining : events) {
        SecurelyClearString(&remaining.payload);
      }
      return Status::Ok();
    }
    route = web_surface_route_;
    web_view_bridge = web_view_bridge_.get();
    browser_service_bridge = browser_service_bridge_.get();
  }
  // Relay the whole flush against the snapshot above. Dismissing mid-batch
  // would detach the logical route the remaining messages are validated
  // against and drop them, which is exactly what stalls multi-message
  // private-server joins until an unrelated later page switch retries them.
  bool saw_relayed_join = false;
  bool launch_queued = false;
  for (std::size_t index = 0; index < events.size(); ++index) {
    WebViewHelperEvent& event = events[index];
    const bool hybrid_command =
        event.type == WebViewHelperEventType::kExecuteRoblox ||
        event.type == WebViewHelperEventType::kRobloxWkHybrid;
    // Private joins are translated and launched by the host: the app cannot
    // serve them, so relaying would only poison its queue with a join that
    // can unblock on a later page switch and relaunch the game after the
    // user left it.
    const bool private_join =
        hybrid_command && IsRobloxWebViewPrivateJoinCommand(event.payload);
    // Every other launchGame is parsed here as well. Roblox defers a signalled
    // launchGame until the Lua app next navigates, so relaying one stalls the
    // join until the user clicks an unrelated sidebar tab. A request the host
    // starts itself is never forwarded, so the deferred copy Roblox may still
    // be holding cannot start a second join later.
    RobloxExperienceLaunchRequest parsed_request;
    const bool host_launch =
        hybrid_command && !private_join &&
        ParseRobloxWebViewLaunchGame(event.payload, &parsed_request).ok();

    Status route_status = Status::Ok();
    bool relayed = false;
    if (event.type == WebViewHelperEventType::kRobloxCookie) {
      route_status = AcceptWebViewRobloxCookie(event.payload);
    } else if (hybrid_command) {
      RobloxExperienceLaunchRequest request;
      bool have_request = false;
      if (private_join) {
        // The app rejects private/reserved joins (its Game handler only
        // serves RequestGame), so translate the command into an
        // experience-launch request the host starts itself. A payload that
        // cannot be translated leaves the window open so the page keeps its
        // response channel and the user can navigate back.
        const Status translate_status =
            TranslateRobloxWebViewPrivateJoinToLaunchRequest(event.payload,
                                                             &request);
        have_request = translate_status.ok();
        if (!have_request) {
          std::fprintf(stderr,
                       "  [webview] private game join ignored: %s\n",
                       translate_status.message().c_str());
        }
      } else if (host_launch) {
        request = std::move(parsed_request);
        have_request = true;
      } else {
        // Roblox serves this command, so relay it. A relayed join still
        // needs the dismissal below.
        if (IsRobloxWebViewGameLaunchCommand(event.payload)) {
          saw_relayed_join = true;
        }
        route_status = RouteWebSurfaceEvent(route, event, web_view_bridge);
        relayed = true;
      }
      if (have_request) {
        bool duplicate = false;
        const Status dispatch_status = DispatchWebJoinLaunch(request,
                                                             &duplicate);
        if (dispatch_status.ok()) {
          // A redelivered duplicate queues nothing, so it must not trigger
          // the dismissal either.
          launch_queued = !duplicate;
        } else if (!private_join) {
          // The host could not start this one after all, so relay it instead
          // of dropping the click. Roblox defers it until the app navigates,
          // which is the stall this path exists to avoid, but losing the join
          // outright is worse than the old behaviour. It parsed as a join, so
          // the dismissal below still applies to it.
          saw_relayed_join = true;
          route_status = RouteWebSurfaceEvent(route, event, web_view_bridge);
          relayed = true;
        }
      }
    } else {
      route_status = RouteWebSurfaceEvent(route, event, web_view_bridge);
      relayed = true;
    }
    // A solved challenge still has to reach Roblox: it carries the callback
    // acknowledgement that lets the login flow continue. Forwarding it does
    // not dismiss the surface, so the batch below is unaffected.
    if (route_status.ok() && relayed &&
        IsRobloxWebViewChallengeSuccess(event.payload)) {
      route_status = HandleSolvedChallenge(logical_generation);
    }
    SecurelyClearString(&event.payload);
    if (!route_status.ok()) {
      for (WebViewHelperEvent& remaining : events) {
        SecurelyClearString(&remaining.payload);
      }
      return route_status;
    }
  }
  if (!saw_relayed_join && !launch_queued) {
    return Status::Ok();
  }
  // Every join command is already handled: host-launched ones were dispatched
  // above, and any relayed one has reached Roblox. Closing the surface queues
  // the window-gone event the app waits on (WebView.handleWindowClose for a
  // MessageBus window, DataModel focus hand-back for a notification window,
  // BrowserWindowClosed for a BrowserService window), but queued events are
  // only published by the bridge drains. Without an immediate drain the join
  // stays queued until an unrelated later page switch pumps the queue again. A
  // failed dismissal only affects presentation, so it must not abort the
  // platform event drain that keeps the running app alive.
  (void)DismissWebSurfaceForGameLaunch(source_process, process_generation,
                                       logical_generation);
  if (web_view_bridge != nullptr) {
    const Status close_status = web_view_bridge->DrainHostWindowEvents();
    if (!close_status.ok()) {
      std::fprintf(stderr,
                   "  [webview] could not deliver the in-app browser close "
                   "for the experience: %s\n",
                   close_status.message().c_str());
    }
  }
  if (browser_service_bridge != nullptr) {
    const Status browser_status =
        browser_service_bridge->DrainOutgoingEvents();
    if (!browser_status.ok()) {
      std::fprintf(stderr,
                   "  [browser] could not deliver the browser window close "
                   "for the experience: %s\n",
                   browser_status.message().c_str());
    }
  }
  return Status::Ok();
}

Status RobloxExperienceComposition::DismissWebSurfaceForGameLaunch(
    const std::shared_ptr<WebViewHelperProcess>& source_process,
    uint64_t process_generation, uint64_t logical_generation) {
  std::lock_guard<std::mutex> operation_lock(web_surface_operation_mutex_);
  std::shared_ptr<WebViewHelperProcess> process;
  WebViewHelperExitObserver exit_observer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source_process != web_surface_process_ || process_generation == 0 ||
        process_generation != web_surface_process_generation_ ||
        logical_generation == 0 ||
        logical_generation != web_surface_logical_generation_ ||
        web_surface_route_ == WebSurfaceRoute::kNone) {
      // A superseded or already dismissed surface must not close the window
      // that replaced it.
      return Status::Ok();
    }
    // Detach the logical route but keep the helper process alive: killing it
    // here would drop in-flight handshake packets of a multi-message join and
    // force the next surface to respawn. The next OpenWebSurface reuses the
    // hidden process via LoadUrl, which navigates away from the old document.
    process = web_surface_process_;
    exit_observer = std::move(web_surface_logical_exit_observer_);
    web_surface_route_ = WebSurfaceRoute::kNone;
    web_surface_logical_generation_ = 0;
  }
  // Roblox's Android WebViewProtocol leaves the browser window as part of the
  // join transition and only starts the queued experience once the host reports
  // the surface gone: WebView.handleWindowClose for a MessageBus window, and a
  // DataModel focus hand-back for a notification window such as the game
  // servers page.
  std::fprintf(stderr,
               "  [webview] game join relayed; dismissing the in-app browser "
               "window for the experience\n");
  if (process != nullptr && process->running() && !process->SetVisible(false)) {
    std::fprintf(stderr,
                 "  [webview] could not hide the in-app browser window\n");
  }
  if (exit_observer.valid()) {
    exit_observer.on_exit(exit_observer.context.get());
  }
  return Status::Ok();
}


Status RobloxExperienceComposition::DrainPlatformEvents() {
  RobloxWebViewBridge* web_view_bridge = nullptr;
  RobloxBrowserServiceBridge* browser_service_bridge = nullptr;
  std::shared_ptr<WebViewHelperProcess> web_surface_process;
  uint64_t process_generation = 0;
  uint64_t logical_generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    web_view_bridge = web_view_bridge_.get();
    browser_service_bridge = browser_service_bridge_.get();
    web_surface_process = web_surface_process_;
    process_generation = web_surface_process_generation_;
    logical_generation = web_surface_logical_generation_;
  }
  const auto drain_helper =
      [&](const std::shared_ptr<WebViewHelperProcess>& process,
          uint64_t process_generation, uint64_t logical_generation) -> Status {
    if (process == nullptr || !process->running()) {
      return Status::Ok();
    }
    std::vector<WebViewHelperEvent> events;
    if (!process->DrainEvents(&events)) {
      return process->running()
                 ? Unavailable("could not receive WebView helper events")
                 : Status::Ok();
    }
    // The batch relays every message drained in this flush before the join
    // dismissal detaches the route, and publishes the resulting window-gone
    // event on the same tick.
    return RouteWebSurfaceEventBatch(process, process_generation,
                                     logical_generation, events);
  };
  // The browser window dismissal unblocks the queued join already waiting in
  // the app: the close/return event must be observable on the same tick that
  // relayed the join command. A dismissal queued here is only published the
  // NEXT time this drain runs, which defers the join until an unrelated later
  // event (e.g. a page switch) pumps the queue again, so helper events go
  // first and host-window events are re-drained immediately after.
  Status status = drain_helper(web_surface_process, process_generation,
                               logical_generation);
  if (status.ok() && web_view_bridge != nullptr) {
    status = web_view_bridge->DrainHostWindowEvents();
  }
  if (status.ok()) {
    status = CloseUnclaimedSolvedChallenge();
  }
  if (status.ok() && browser_service_bridge != nullptr) {
    status = browser_service_bridge->DrainOutgoingEvents();
  }
  if (status.ok()) {
    status = PromoteAuthenticatedSession();
  }
  if (status.ok()) {
    status = RefreshLateSurface();
  }
  return status;
}

Status RobloxExperienceComposition::PromoteAuthenticatedSession() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subscribed_ || !platform_protocols_initialized_) {
      return Status::Ok();
    }
  }

  jnivm::VM* vm = jnivm::VM::FromJavaVM(environment_.java_vm);
  if (vm == nullptr) {
    return Status::Ok();
  }
  const jnivm::RobloxAuthIdentity identity =
      vm->GetRobloxAuthIdentitySnapshot();
  if (identity.user_id <= 0 || identity.username.empty()) {
    return Status::Ok();
  }
  std::string credential;
  if (!vm->CopyRobloxCredentialFromProvider(&credential) ||
      credential.empty()) {
    SecurelyClearString(&credential);
    return Status::Ok();
  }
  SecurelyClearString(&credential);
  const GameSurface surface =
      surface_provider_.snapshot(surface_provider_.context);
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal.kind = GameSessionPrincipalKind::kAuthenticated;
  readiness.principal.generation = 1;
  readiness.principal.principal_id = std::to_string(identity.user_id);
  readiness.principal.base_url = kRobloxBaseUrl;
  readiness.surface = surface;
  readiness.username = identity.username;
  if (!readiness.complete()) {
    return Status::Ok();
  }

  Status status = OnLuaAppReady(std::move(readiness));
  if (!status.ok()) {
    return status;
  }
  if (consumes_window_surface_events_) {
    window::WindowSurfaceEvent stale_event;
    while (window::PollWindowSurfaceEvent(&stale_event)) {
      // Readiness already captured the latest snapshot; discard older events.
    }
  }
  vm->SetRobloxExperienceLifecycleCallbacks(
      lifecycle_target_,
      jnivm::RobloxExperienceLifecycleCallbacks{
          &RobloxExperienceComposition::NotifyLateLuaAppDidReturn});
  {
    std::lock_guard<std::mutex> lock(mutex_);
    late_lifecycle_vm_ = vm;
    late_surface_tracking_ = true;
    late_surface_snapshot_ = surface;
  }
  std::fprintf(stderr,
               "  [experience] native sign-in completed; dynamic launch "
               "requests subscribed without restart\n");
  return Status::Ok();
}

Status RobloxExperienceComposition::RefreshLateSurface() {
  GameSurface previous;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!late_surface_tracking_) {
      return Status::Ok();
    }
    previous = late_surface_snapshot_;
  }
  if (consumes_window_surface_events_) {
    window::WindowSurfaceEvent event;
    while (window::PollWindowSurfaceEvent(&event)) {
      Status status = Status::Ok();
      switch (event.type) {
        case window::WindowSurfaceEventType::kCreated:
          status =
              SurfaceUpdateStatus(SurfaceCreated(event.surface.generation));
          break;
        case window::WindowSurfaceEventType::kChanged:
          status = SurfaceUpdateStatus(
              SurfaceChanged({event.surface.generation,
                              event.surface.native_window, event.surface.width,
                              event.surface.height, event.surface.dpi_scale}));
          break;
        case window::WindowSurfaceEventType::kDestroyed:
          status =
              SurfaceUpdateStatus(SurfaceDestroyed(event.surface.generation));
          break;
      }
      if (!status.ok()) {
        return status;
      }
      status = window::RecordResizeReadinessSurfaceCommit(event);
      if (!status.ok()) {
        return status;
      }
      const GameSurface committed =
          event.type == window::WindowSurfaceEventType::kDestroyed
              ? GameSurface{event.surface.generation, 0, 0, 0}
              : GameSurface{event.surface.generation,
                            event.surface.native_window, event.surface.width,
                            event.surface.height, event.surface.dpi_scale};
      std::lock_guard<std::mutex> lock(mutex_);
      late_surface_snapshot_ = committed;
    }
    return Status::Ok();
  }
  const GameSurface observed =
      surface_provider_.snapshot(surface_provider_.context);
  if (SameSurface(previous, observed)) {
    return Status::Ok();
  }

  Status status = Status::Ok();
  if (observed.native_window == 0 || observed.width == 0 ||
      observed.height == 0 || observed.generation == 0) {
    if (previous.native_window != 0 && previous.generation != 0) {
      status = SurfaceUpdateStatus(SurfaceDestroyed(previous.generation));
    }
  } else if (previous.native_window == 0 ||
             previous.generation != observed.generation) {
    if (previous.native_window != 0 && previous.generation != 0) {
      status = SurfaceUpdateStatus(SurfaceDestroyed(previous.generation));
    }
    if (status.ok()) {
      status = SurfaceUpdateStatus(SurfaceCreated(observed.generation));
    }
    if (status.ok()) {
      status = SurfaceUpdateStatus(SurfaceChanged(observed));
    }
  } else {
    status = SurfaceUpdateStatus(SurfaceChanged(observed));
  }
  if (status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    late_surface_snapshot_ = observed;
  }
  return status;
}

void RobloxExperienceComposition::NotifyLateLuaAppDidReturn(void* context) {
  auto* target = static_cast<LifecycleTarget*>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->composition != nullptr) {
    target->composition->NotifyLuaAppDidReturn();
  }
}

bool RobloxExperienceComposition::CanHandleMainTextPasteLocked() const {
  return subscribed_ && controller_ != nullptr && objects_ != nullptr &&
         !game_active_ && !launch_in_progress_ && active_launch_ == nullptr &&
         pending_launch_requests_.empty() && !lua_app_return_pending_ &&
         !lua_app_surface_recreation_pending_ &&
         !controlled_switch_waiting_for_return_;
}

bool RobloxExperienceComposition::CanHandleMainTextPaste() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return CanHandleMainTextPasteLocked();
}

Status RobloxExperienceComposition::QueuePrivateServerLaunch(
    int64_t place_id, std::string_view link_code) {
  if (place_id <= 0 || link_code.empty() || link_code.size() > 128) {
    return Invalid("private-server paste launch identity is invalid");
  }
  for (const unsigned char character : link_code) {
    if (character < '0' || character > '9') {
      return Invalid("private-server paste link code is invalid");
    }
  }

  const std::string owned_link_code(link_code);
  const nlohmann::json document = {
      {"placeId", place_id},
      {"linkCode", owned_link_code},
      {"joinAttemptOrigin", "PrivateServerPaste"},
  };
  RobloxExperienceLaunchRequest request;
  Status parse_status =
      ParseRobloxExperienceLaunchJson(document.dump(), &request);
  if (!parse_status.ok()) return parse_status;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!CanHandleMainTextPasteLocked()) {
    return FailedPrecondition(
        "private-server paste launch requires the main LuaApp surface");
  }
  const Status status = QueueLaunchRequestLocked(request);
  if (status.ok()) {
    std::fprintf(stderr,
                 "  [text] private-server paste confirmed; queued place_id=%lld\n",
                 static_cast<long long>(place_id));
  }
  return status;
}

Status RobloxExperienceComposition::QueueLaunchRequestLocked(
    const RobloxExperienceLaunchRequest& request) {
  if (!subscribed_ || controller_ == nullptr || objects_ == nullptr) {
    return FailedPrecondition("experience composition is not subscribed");
  }
  if (!request.canonical_json.empty()) {
    const bool active_game_matches =
        game_active_ && request.canonical_json == active_game_canonical_json_;
    const bool active_launch_matches =
        active_launch_ != nullptr &&
        request.canonical_json == active_launch_->request.canonical_json;
    bool pending_launch_matches = false;
    for (const RobloxExperienceLaunchRequest& pending :
         pending_launch_requests_) {
      if (request.canonical_json == pending.canonical_json) {
        pending_launch_matches = true;
        break;
      }
    }
    if (active_game_matches || active_launch_matches ||
        pending_launch_matches) {
      std::fprintf(stderr,
                   "[experience] duplicate launch already active or queued; "
                   "request acknowledged\n");
      return Status::Ok();
    }
  }
  if (pending_launch_requests_.size() >= kMaxPendingLaunchRequests) {
    return Status::Error(StatusCode::kUnavailable,
                         "experience launch request queue is full");
  }
  pending_launch_requests_.push_back(request);
  return Status::Ok();
}

Status RobloxExperienceComposition::Dispatch(
    const RobloxExperienceLaunchRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  return QueueLaunchRequestLocked(request);
}

Status RobloxExperienceComposition::DispatchWebJoinLaunch(
    const RobloxExperienceLaunchRequest& request, bool* duplicate) {
  if (duplicate != nullptr) {
    *duplicate = false;
  }
  if (!request.join_attempt_id.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.join_attempt_id == last_web_join_attempt_id_) {
      // Same click redelivered (hidden-page retry, helper flush, superseded
      // surface): not a new user gesture. Re-dispatching it after the user
      // left the game would relaunch the server on the next page switch, once
      // the leave cleared the active/pending dedup state. A fresh click
      // carries a fresh id.
      if (duplicate != nullptr) {
        *duplicate = true;
      }
      std::fprintf(stderr,
                   "  [webview] duplicate web game join suppressed\n");
      return Status::Ok();
    }
  }
  const Status status = Dispatch(request);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] could not queue the web game join: %s\n",
                 status.message().c_str());
    return status;
  }
  std::fprintf(stderr,
               "  [webview] web game join launched by the host; launching the "
               "experience\n");
  if (!request.join_attempt_id.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_web_join_attempt_id_ = request.join_attempt_id;
  }
  return Status::Ok();
}

void RobloxExperienceComposition::DispatchLaunchKeptByMessageBus() {
  RobloxExperienceLaunchBridge* bridge = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bridge = bridge_.get();
  }
  if (bridge == nullptr) {
    return;
  }
  RobloxExperienceLaunchRequest request;
  bool found = false;
  Status status = bridge->ReadLastLaunch(&request, &found);
  if (!status.ok()) {
    std::fprintf(stderr,
                 "[experience] could not read Roblox's last launch request: "
                 "%s\n",
                 status.message().c_str());
    return;
  }
  if (!found) {
    return;
  }
  std::fprintf(stderr,
               "[experience] starting the launch Roblox sent while leaving "
               "the game place_id=%lld\n",
               static_cast<long long>(request.place_id));
  status = Dispatch(request);
  if (!status.ok()) {
    std::fprintf(stderr, "[experience] launch request rejected: %s\n",
                 status.message().c_str());
  }
}

Status RobloxExperienceComposition::DrainExternalLaunchRequests() {
  std::size_t available = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || controller_ == nullptr || objects_ == nullptr) {
      return FailedPrecondition("experience composition is not subscribed");
    }
    available = kMaxPendingLaunchRequests - pending_launch_requests_.size();
  }
  Status status = DrainActiveExternalLaunchRequests(
      ExternalLaunchSink{this, &RobloxExperienceComposition::DispatchLaunch},
      available);
  // A MessageBus race can fill the queue; leave the request at the broker's
  // front and retry on the next drain.
  return status.code() == StatusCode::kUnavailable ? Status::Ok() : status;
}

void RobloxExperienceComposition::NotifyLuaAppDidReturn() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (subscribed_)
    lua_app_return_pending_ = true;
}

Status RobloxExperienceComposition::DrainLaunchRequests() {
  Status completed_result = Status::Ok();
  if (launch_worker_.joinable()) {
    const OwnedPthreadWaitResult wait = launch_worker_.WaitFor(0, 1);
    if (wait.status == OwnedPthreadWaitStatus::kTimedOut)
      return Status::Ok();
    if (!wait.joined()) {
      return Status::Error(StatusCode::kPlatformError,
                           "could not reap experience launch worker: " +
                               std::to_string(wait.platform_error));
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      launch_in_progress_ = false;
      if (active_launch_ != nullptr) {
        completed_result = active_launch_->result;
        game_active_ = completed_result.ok();
        window::SetGameSessionActive(game_active_);
        active_game_canonical_json_ =
            completed_result.ok() ? active_launch_->request.canonical_json
                                  : std::string();
        active_launch_.reset();
      }
      if (!completed_result.ok()) {
        pending_launch_requests_.clear();
        presence_request_.reset();
        playing_presence_published_ = false;
      }
    }
    if (!completed_result.ok()) {
      NotifyPresence(RobloxExperiencePresencePhase::kBrowsing, nullptr);
      return completed_result;
    }
  }

  std::shared_ptr<RobloxFreshGameLaunchController> returned_controller;
  bool switched_by_the_host = false;
  bool had_a_running_game = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lua_app_return_pending_ && !launch_in_progress_ &&
        active_launch_ == nullptr) {
      // Consume first so a concurrent callback can set a new observation.
      lua_app_return_pending_ = false;
      returned_controller = controller_;
      // Read both before the block below clears them: a queued switch leaves
      // the experience too, and only a leave the host did not ask for is the
      // player choosing to leave.
      switched_by_the_host = controlled_switch_waiting_for_return_;
      had_a_running_game = game_active_;
    }
  }
  if (returned_controller != nullptr) {
    const Status observed = returned_controller->ObserveLuaAppReturn();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!observed.ok()) {
        lua_app_return_pending_ = true;
        pending_launch_requests_.clear();
        return observed;
      }
      game_active_ = false;
      window::SetGameSessionActive(false);
      active_game_canonical_json_.clear();
      presence_request_.reset();
      playing_presence_published_ = false;
      controlled_switch_waiting_for_return_ = false;
      if (had_a_running_game && !switched_by_the_host) {
        player_left_experience_ = true;
      }
    }
    NotifyPresence(RobloxExperiencePresencePhase::kBrowsing, nullptr);
    DispatchLaunchKeptByMessageBus();
  }

  Status external_launch_status = DrainExternalLaunchRequests();
  if (!external_launch_status.ok())
    return external_launch_status;

  bool switch_active_game = false;
  std::optional<RobloxExperienceLaunchRequest> joining_request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || controller_ == nullptr || objects_ == nullptr) {
      return FailedPrecondition("experience composition is not subscribed");
    }
    if (controlled_switch_waiting_for_return_)
      return Status::Ok();
    if (!launch_in_progress_ && active_launch_ == nullptr && game_active_ &&
        !pending_launch_requests_.empty()) {
      // Blocks a reentrant drain while the leave pumps main-thread messages.
      // The leave ends in the Native stage with no Lua app return, so the
      // queued launch starts on the next drain.
      controlled_switch_waiting_for_return_ = true;
      switch_active_game = true;
    }
  }
  if (switch_active_game) {
    std::fprintf(stderr,
                 "[experience] leaving active game for queued launch\n");
    const Status leave_status = LeaveGame();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      controlled_switch_waiting_for_return_ = false;
    }
    return leave_status;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || controller_ == nullptr || objects_ == nullptr) {
      return FailedPrecondition("experience composition is not subscribed");
    }
    if (launch_in_progress_ || active_launch_ != nullptr)
      return Status::Ok();
    if (game_active_)
      return Status::Ok();
    if (pending_launch_requests_.empty())
      return Status::Ok();
    if (next_request_id_ == 0 ||
        next_request_id_ == std::numeric_limits<uint64_t>::max()) {
      pending_launch_requests_.clear();
      return FailedPrecondition(
          "experience launch request id space is exhausted");
    }
    auto task = std::make_unique<LaunchTask>();
    task->request = std::move(pending_launch_requests_.front());
    pending_launch_requests_.pop_front();
    task->controller = controller_;
    task->context.request_id = next_request_id_++;
    task->context.native_gl_class = objects_->native_gl_class;
    task->context.activity = objects_->activity;
    task->context.vr_context = objects_->activity;
    task->context.principal = readiness_.principal;
    task->context.game_surface = readiness_.surface;
    task->context.username = readiness_.username;
    task->context.is_under_13 = readiness_.is_under_13;
    task->context.join_request_type = JoinRequestTypeFor(task->request);
    std::fprintf(stderr,
                 "[experience] launching place_id=%lld join_request_type=%d\n",
                 static_cast<long long>(task->request.place_id),
                 task->context.join_request_type);
    active_launch_ = std::move(task);
    launch_in_progress_ = true;
    presence_request_ = active_launch_->request;
    playing_presence_published_ = false;
    joining_request = active_launch_->request;
  }

  NotifyPresence(RobloxExperiencePresencePhase::kJoining, &*joining_request);

  const int start_error =
      launch_worker_.Start(&RobloxExperienceComposition::RunLaunchWorker, this,
                           kLaunchWorkerStackSize);
  if (start_error == 0)
    return Status::Ok();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    launch_in_progress_ = false;
    active_launch_.reset();
    presence_request_.reset();
    active_game_canonical_json_.clear();
    playing_presence_published_ = false;
    pending_launch_requests_.clear();
  }
  NotifyPresence(RobloxExperiencePresencePhase::kBrowsing, nullptr);
  return Status::Error(StatusCode::kPlatformError,
                       "could not start experience launch worker: " +
                           std::to_string(start_error));
}

void* RobloxExperienceComposition::RunLaunchWorker(void* context) {
  if (context != nullptr) {
    static_cast<RobloxExperienceComposition*>(context)->RunActiveLaunch();
  }
  return nullptr;
}

void RobloxExperienceComposition::GamePresented(void* context,
                                                uint64_t frame_serial) {
  if (context != nullptr && frame_serial != 0) {
    static_cast<RobloxExperienceComposition*>(context)
        ->PublishPresentedPresence(frame_serial);
  }
}

void RobloxExperienceComposition::PublishPresentedPresence(
    uint64_t frame_serial) {
  std::optional<RobloxExperienceLaunchRequest> request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || playing_presence_published_ ||
        !presence_request_.has_value()) {
      return;
    }
    playing_presence_published_ = true;
    request = presence_request_;
  }
  std::fprintf(stderr,
               "  [experience] first game frame published to presence "
               "frame=%llu place_id=%lld\n",
               static_cast<unsigned long long>(frame_serial),
               static_cast<long long>(request->place_id));
  NotifyPresence(RobloxExperiencePresencePhase::kPlaying, &*request);
}

void RobloxExperienceComposition::RunActiveLaunch() {
  LaunchTask* task = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    task = active_launch_.get();
  }
  if (task == nullptr)
    return;
  Status status =
      BuildLaunchObjects(task->context.game_surface, &task->context.surface,
                         &task->context.platform_params);
  if (status.ok()) {
    task->context.device_params = nullptr;
    status = task->controller->Launch(task->request, task->context);
  }
  if (task->context.surface != nullptr ||
      task->context.platform_params != nullptr) {
    JNIEnv* env = nullptr;
    const Status acquire = environment_.Acquire(&env);
    if (acquire.ok()) {
      if (task->context.platform_params != nullptr)
        env->DeleteLocalRef(task->context.platform_params);
      if (task->context.surface != nullptr)
        env->DeleteLocalRef(task->context.surface);
    } else if (status.ok()) {
      status = acquire;
    }
  }
  task->result = status;
}

Status RobloxExperienceComposition::LeaveGame() {
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    controller = controller_;
  }
  if (controller == nullptr) {
    return FailedPrecondition("experience controller is unavailable");
  }
  const Status status = controller->Leave();
  if (status.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    game_active_ = false;
    window::SetGameSessionActive(false);
    active_game_canonical_json_.clear();
  }
  return status;
}

GameSessionUpdateResult RobloxExperienceComposition::SurfaceCreated(
    uint64_t generation) {
  std::lock_guard<std::mutex> operation_lock(surface_operation_mutex_);
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  bool forward = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    controller = controller_;
    forward = game_active_;
    if (!forward && generation != 0) {
      if (readiness_.surface.generation != generation) {
        lua_app_surface_recreation_pending_ = true;
      }
      readiness_.surface.generation = generation;
    }
  }
  return forward && controller != nullptr
             ? controller->SurfaceCreated(generation)
             : AcceptedLuaAppSurface("LuaApp surface generation retained");
}

GameSessionUpdateResult RobloxExperienceComposition::SurfaceChanged(
    GameSurface surface) {
  std::lock_guard<std::mutex> operation_lock(surface_operation_mutex_);
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  bool forward = false;
  bool recreate_lua_app = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    controller = controller_;
    forward = game_active_;
    recreate_lua_app = !forward && lua_app_surface_recreation_pending_;
  }
  GameSessionUpdateResult result =
      forward && controller != nullptr
          ? controller->RebindSurface(surface)
          : AcceptedLuaAppSurface("LuaApp surface rebound");
  if (!forward) {
    const Status status = recreate_lua_app ? RestartLuaAppSurface(surface)
                                           : RebindLuaAppSurface(surface);
    if (!status.ok()) {
      return RejectedLuaAppSurface(status);
    }
  }
  if (result.ok()) {
    std::lock_guard<std::mutex> lock(mutex_);
    readiness_.surface = surface;
    if (!forward) {
      lua_app_surface_recreation_pending_ = false;
    }
  }
  return result;
}

GameSessionUpdateResult RobloxExperienceComposition::SurfaceDestroyed(
    uint64_t generation) {
  std::lock_guard<std::mutex> operation_lock(surface_operation_mutex_);
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  bool forward = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    controller = controller_;
    forward = game_active_;
    if (!forward && readiness_.surface.generation == generation) {
      lua_app_surface_recreation_pending_ = true;
      readiness_.surface.native_window = 0;
      readiness_.surface.width = 0;
      readiness_.surface.height = 0;
    }
  }
  return forward && controller != nullptr
             ? controller->SurfaceDestroyed(generation)
             : AcceptedLuaAppSurface("LuaApp surface destruction retained");
}

Status RobloxExperienceComposition::Shutdown() {
  std::lock_guard<std::mutex> surface_operation_lock(surface_operation_mutex_);
  std::unique_ptr<RobloxExperienceLaunchBridge> bridge;
  std::unique_ptr<RobloxWebViewBridge> web_view_bridge;
  std::unique_ptr<RobloxBrowserServiceBridge> browser_service_bridge;
  std::unique_ptr<RobloxPermissionsBridge> permissions_bridge;
  std::unique_ptr<RobloxCallProtocolBridge> call_protocol_bridge;
  std::unique_ptr<RobloxAccountBridge> account_bridge;
  std::shared_ptr<WebViewHelperProcess> web_surface_process;
  jnivm::VM* late_lifecycle_vm = nullptr;
  {
    std::lock_guard<std::mutex> target_lock(lifecycle_target_->mutex);
    lifecycle_target_->composition = nullptr;
  }
  {
    std::lock_guard<std::mutex> target_lock(web_surface_exit_target_->mutex);
    web_surface_exit_target_->composition = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    subscribed_ = false;
    game_active_ = false;
    window::SetGameSessionActive(false);
    active_game_canonical_json_.clear();
    presence_request_.reset();
    playing_presence_published_ = false;
    lua_app_return_pending_ = false;
    controlled_switch_waiting_for_return_ = false;
    player_left_experience_ = false;
    lua_app_surface_recreation_pending_ = false;
    pending_launch_requests_.clear();
    last_web_join_attempt_id_.clear();
    bridge = std::move(bridge_);
    web_view_bridge = std::move(web_view_bridge_);
    browser_service_bridge = std::move(browser_service_bridge_);
    permissions_bridge = std::move(permissions_bridge_);
    call_protocol_bridge = std::move(call_protocol_bridge_);
    account_bridge = std::move(account_bridge_);
    web_surface_process = std::move(web_surface_process_);
    web_surface_logical_exit_observer_ = {};
    web_surface_route_ = WebSurfaceRoute::kNone;
    web_surface_process_generation_ = 0;
    web_surface_logical_generation_ = 0;
    late_surface_tracking_ = false;
    late_surface_snapshot_ = {};
    late_lifecycle_vm = late_lifecycle_vm_;
    late_lifecycle_vm_ = nullptr;
  }
  if (late_lifecycle_vm != nullptr) {
    late_lifecycle_vm->ClearRobloxExperienceLifecycleCallbacks();
  }
  if (web_surface_process != nullptr) {
    (void)web_surface_process->RequestClose();
  }
  Status status = call_protocol_bridge != nullptr
                      ? call_protocol_bridge->Shutdown()
                      : Status::Ok();
  const Status account_status =
      account_bridge != nullptr ? account_bridge->Shutdown() : Status::Ok();
  if (status.ok())
    status = account_status;
  const Status permissions_status = permissions_bridge != nullptr
                                        ? permissions_bridge->Shutdown()
                                        : Status::Ok();
  if (status.ok())
    status = permissions_status;
  const Status browser_status = browser_service_bridge != nullptr
                                    ? browser_service_bridge->Shutdown()
                                    : Status::Ok();
  if (status.ok())
    status = browser_status;
  const Status web_view_status =
      web_view_bridge != nullptr ? web_view_bridge->Shutdown() : Status::Ok();
  if (status.ok()) {
    status = web_view_status;
  }
  const Status bridge_status =
      bridge != nullptr ? bridge->Shutdown() : Status::Ok();
  if (status.ok()) {
    status = bridge_status;
  }
  browser_service_bridge.reset();
  call_protocol_bridge.reset();
  account_bridge.reset();
  permissions_bridge.reset();
  web_view_bridge.reset();
  bridge.reset();
  if (launch_worker_.joinable()) {
    const OwnedPthreadWaitResult wait = launch_worker_.WaitFor(-1, 1);
    if (!wait.joined() && status.ok()) {
      status = Status::Error(StatusCode::kPlatformError,
                             "could not join experience launch worker: " +
                                 std::to_string(wait.platform_error));
    }
  }
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    launch_in_progress_ = false;
    lua_app_return_pending_ = false;
    if (active_launch_ != nullptr) {
      if (status.ok())
        status = active_launch_->result;
      active_launch_.reset();
    }
    controller = std::move(controller_);
  }
  if (controller != nullptr) {
    const Status controller_status = controller->Shutdown();
    if (status.ok())
      status = controller_status;
  }
  const Status release = ReleaseGlobalObjects();
  return status.ok() ? release : status;
}

bool RobloxExperienceComposition::subscribed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subscribed_;
}

GameSessionSnapshot RobloxExperienceComposition::Snapshot() const {
  std::shared_ptr<RobloxFreshGameLaunchController> controller;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    controller = controller_;
  }
  return controller != nullptr ? controller->Snapshot() : GameSessionSnapshot{};
}

bool RobloxExperienceComposition::TakePlayerLeftExperience() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool left = player_left_experience_;
  player_left_experience_ = false;
  return left;
}

std::string RobloxExperienceComposition::ActiveGameInstanceId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return presence_request_.has_value() ? presence_request_->game_instance_id
                                       : std::string();
}

bool RobloxExperienceComposition::InExperience() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return presence_request_.has_value();
}

void RobloxExperienceComposition::NotifyPresence(
    RobloxExperiencePresencePhase phase,
    const RobloxExperienceLaunchRequest* request) const {
  if (presence_observer_.valid()) {
    presence_observer_.notify(presence_observer_.context, phase, request);
  }
}

Status RobloxExperienceComposition::BuildPlatformGlobalObjects() {
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;

  jobject bus_local = nullptr;
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jmethodID singleton =
      bus_class != nullptr
          ? env->GetStaticMethodID(
                bus_class, "f",
                "()Lcom/roblox/universalapp/messagebus/MessageBus;")
          : nullptr;
  bus_local = singleton != nullptr
                  ? env->CallStaticObjectMethod(bus_class, singleton)
                  : nullptr;
  if (bus_class != nullptr)
    env->DeleteLocalRef(bus_class);
  status = bus_local != nullptr
               ? CheckJni(env, "obtain exact MessageBus.f singleton")
               : Unavailable("MessageBus.f singleton is unavailable");

  auto global = std::make_unique<GlobalObjects>();
  if (status.ok()) {
    global->message_bus = env->NewGlobalRef(bus_local);
    if (global->message_bus == nullptr) {
      status = Unavailable("could not retain platform MessageBus JNI root");
    }
  }

  if (bus_local != nullptr)
    env->DeleteLocalRef(bus_local);
  if (!status.ok()) {
    if (global->message_bus != nullptr)
      env->DeleteGlobalRef(global->message_bus);
    return status;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (objects_ != nullptr) {
    env->DeleteGlobalRef(global->message_bus);
    return FailedPrecondition("platform JNI roots are already retained");
  }
  objects_ = std::move(global);
  return Status::Ok();
}

Status RobloxExperienceComposition::BuildLuaAppGlobalObjects() {
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;

  jclass native_gl_local =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  jclass activity_class = nullptr;
  jobject activity_local = nullptr;
  if (native_gl_local == nullptr) {
    status = Unavailable("NativeGLInterface class is unavailable");
  }
  if (status.ok()) {
    status =
        Allocate(env, "android/app/Activity", &activity_class, &activity_local);
  }

  jclass native_gl_global = nullptr;
  jobject activity_global = nullptr;
  if (status.ok()) {
    native_gl_global = static_cast<jclass>(env->NewGlobalRef(native_gl_local));
    activity_global = env->NewGlobalRef(activity_local);
    if (native_gl_global == nullptr || activity_global == nullptr) {
      status = Unavailable("could not retain LuaApp JNI roots");
    }
  }

  if (activity_local != nullptr)
    env->DeleteLocalRef(activity_local);
  if (activity_class != nullptr)
    env->DeleteLocalRef(activity_class);
  if (native_gl_local != nullptr)
    env->DeleteLocalRef(native_gl_local);
  if (!status.ok()) {
    if (activity_global != nullptr)
      env->DeleteGlobalRef(activity_global);
    if (native_gl_global != nullptr)
      env->DeleteGlobalRef(native_gl_global);
    return status;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!platform_protocols_initialized_ || objects_ == nullptr ||
      objects_->message_bus == nullptr ||
      objects_->native_gl_class != nullptr || objects_->activity != nullptr) {
    env->DeleteGlobalRef(activity_global);
    env->DeleteGlobalRef(native_gl_global);
    return FailedPrecondition("platform JNI roots changed before LuaApp setup");
  }
  objects_->native_gl_class = native_gl_global;
  objects_->activity = activity_global;
  return Status::Ok();
}

Status RobloxExperienceComposition::BuildLaunchObjects(
    const GameSurface& surface, jobject* surface_object,
    jobject* platform_params) {
  if (surface_object == nullptr || platform_params == nullptr) {
    return Invalid("fresh launch JNI outputs are null");
  }
  *surface_object = nullptr;
  *platform_params = nullptr;
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;

  jclass surface_class = nullptr;
  jobject surface_local = nullptr;
  status =
      Allocate(env, "android/view/Surface", &surface_class, &surface_local);
  if (status.ok()) {
    const bool populated =
        SetField<jint>(env, surface_local, surface_class, "width", "I",
                       static_cast<jint>(surface.width)) &&
        SetField<jint>(env, surface_local, surface_class, "height", "I",
                       static_cast<jint>(surface.height)) &&
        SetField<jboolean>(env, surface_local, surface_class, "valid", "Z",
                           JNI_TRUE) &&
        SetField<jboolean>(env, surface_local, surface_class, "isValid", "Z",
                           JNI_TRUE);
    status = populated ? CheckJni(env, "populate fresh launch Surface")
                       : Unavailable("required Surface fields are unavailable");
  }

  jclass params_class = nullptr;
  jobject params_local = nullptr;
  if (status.ok()) {
    status = Allocate(env, "com/roblox/engine/jni/model/PlatformParams",
                      &params_class, &params_local);
  }
  jstring asset_path = nullptr;
  if (status.ok()) {
    asset_path = env->NewStringUTF(surface_config_.asset_folder_path.c_str());
    const bool populated =
        asset_path != nullptr &&
        SetField<jobject>(env, params_local, params_class, "assetFolderPath",
                          "Ljava/lang/String;", asset_path) &&
        SetField<jfloat>(env, params_local, params_class, "dpiScale", "F",
                         surface.dpi_scale > 0.0f
                             ? surface.dpi_scale
                             : surface_config_.dpi_scale) &&
        SetField<jint>(env, params_local, params_class, "viewportWidthMm", "I",
                       surface_config_.viewport_width_mm) &&
        SetField<jint>(env, params_local, params_class, "viewportHeightMm", "I",
                       surface_config_.viewport_height_mm) &&
        SetField<jboolean>(
            env, params_local, params_class, "isTouchDevice", "Z",
            surface_config_.is_touch_device ? JNI_TRUE : JNI_FALSE) &&
        SetField<jboolean>(
            env, params_local, params_class, "isMouseDevice", "Z",
            surface_config_.is_mouse_device ? JNI_TRUE : JNI_FALSE) &&
        SetField<jboolean>(
            env, params_local, params_class, "isKeyboardDevice", "Z",
            surface_config_.is_keyboard_device ? JNI_TRUE : JNI_FALSE);
    status =
        populated
            ? CheckJni(env, "populate fresh launch PlatformParams")
            : Unavailable("required PlatformParams fields are unavailable");
  }
  if (asset_path != nullptr)
    env->DeleteLocalRef(asset_path);
  if (surface_class != nullptr)
    env->DeleteLocalRef(surface_class);
  if (params_class != nullptr)
    env->DeleteLocalRef(params_class);
  if (!status.ok()) {
    if (params_local != nullptr)
      env->DeleteLocalRef(params_local);
    if (surface_local != nullptr)
      env->DeleteLocalRef(surface_local);
    return status;
  }
  *surface_object = surface_local;
  *platform_params = params_local;
  return Status::Ok();
}

Status RobloxExperienceComposition::BuildLuaAppStartParams(
    JNIEnv* env, const RobloxLuaAppExperienceReadiness& readiness,
    jobject surface_object, jobject platform_params, jobject activity,
    jobject* start_app_params) {
  if (env == nullptr || surface_object == nullptr ||
      platform_params == nullptr || start_app_params == nullptr) {
    return Invalid("LuaApp StartAppParams inputs are incomplete");
  }
  *start_app_params = nullptr;

  int64_t user_id = 0;
  const char* begin = readiness.principal.principal_id.data();
  const char* end = begin + readiness.principal.principal_id.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, user_id);
  if (parsed.ec != std::errc() || parsed.ptr != end || user_id <= 0) {
    return Invalid("LuaApp principal id is not a positive integer");
  }

  jclass params_class = nullptr;
  jobject params_local = nullptr;
  Status status =
      Allocate(env, "com/roblox/engine/jni/autovalue/StartAppParams",
               &params_class, &params_local);
  jstring app_starter_place = nullptr;
  jstring app_starter_script = nullptr;
  jstring username = nullptr;
  jstring selected_theme = nullptr;
  if (status.ok()) {
    app_starter_place = env->NewStringUTF("rbxasset://places/Mobile.rbxl");
    app_starter_script = env->NewStringUTF("LuaAppStarterScript");
    username = env->NewStringUTF(readiness.username.c_str());
    const char* theme = std::getenv("MOCKTAIL_RESOLVED_THEME_INTERNAL");
    selected_theme = env->NewStringUTF(theme != nullptr ? theme : "Dark");
    const bool populated =
        app_starter_place != nullptr && app_starter_script != nullptr &&
        username != nullptr && selected_theme != nullptr &&
        SetField<jobject>(env, params_local, params_class, "surface",
                          "Landroid/view/Surface;", surface_object) &&
        SetField<jobject>(env, params_local, params_class, "platformParams",
                          "Lcom/roblox/engine/jni/model/PlatformParams;",
                          platform_params) &&
        SetField<jobject>(env, params_local, params_class, "appStarterPlace",
                          "Ljava/lang/String;", app_starter_place) &&
        SetField<jobject>(env, params_local, params_class, "appStarterScript",
                          "Ljava/lang/String;", app_starter_script) &&
        SetField<jlong>(env, params_local, params_class, "appUserId", "J",
                        static_cast<jlong>(user_id)) &&
        SetField<jboolean>(env, params_local, params_class, "isUnder13", "Z",
                           readiness.is_under_13 ? JNI_TRUE : JNI_FALSE) &&
        SetField<jobject>(env, params_local, params_class, "username",
                          "Ljava/lang/String;", username) &&
        SetField<jint>(env, params_local, params_class, "membershipType", "I",
                       0) &&
        SetField<jobject>(env, params_local, params_class, "selectedTheme",
                          "Ljava/lang/String;", selected_theme) &&
        SetField<jobject>(env, params_local, params_class, "vrContext",
                          "Landroid/app/Activity;", activity);
    status = populated
                 ? CheckJni(env, "populate LuaApp StartAppParams")
                 : Unavailable(
                       "required LuaApp StartAppParams fields are unavailable");
  }

  if (selected_theme != nullptr)
    env->DeleteLocalRef(selected_theme);
  if (username != nullptr)
    env->DeleteLocalRef(username);
  if (app_starter_script != nullptr)
    env->DeleteLocalRef(app_starter_script);
  if (app_starter_place != nullptr)
    env->DeleteLocalRef(app_starter_place);
  if (params_class != nullptr)
    env->DeleteLocalRef(params_class);
  if (!status.ok()) {
    if (params_local != nullptr)
      env->DeleteLocalRef(params_local);
    return status;
  }
  *start_app_params = params_local;
  return Status::Ok();
}

Status RobloxExperienceComposition::RestartLuaAppSurface(
    const GameSurface& surface) {
  if (surface.generation == 0 || surface.native_window == 0 ||
      surface.width == 0 || surface.height == 0) {
    return Invalid("LuaApp surface recreation requires a complete surface");
  }

  RobloxLuaAppExperienceReadiness readiness;
  jclass native_gl_class = nullptr;
  jobject activity = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || game_active_ || objects_ == nullptr ||
        objects_->native_gl_class == nullptr ||
        !lua_app_surface_recreation_pending_) {
      return FailedPrecondition(
          "LuaApp surface recreation requires active LuaApp JNI roots");
    }
    readiness = readiness_;
    native_gl_class = objects_->native_gl_class;
    activity = objects_->activity;
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;

  jobject surface_object = nullptr;
  jobject platform_params = nullptr;
  status = BuildLaunchObjects(surface, &surface_object, &platform_params);
  jobject start_app_params = nullptr;
  if (status.ok()) {
    status =
        BuildLuaAppStartParams(env, readiness, surface_object, platform_params,
                               activity, &start_app_params);
  }

  jstring stop_reason = nullptr;
  jstring start_reason = nullptr;
  if (status.ok()) {
    stop_reason = env->NewStringUTF("ASMA.stop");
    start_reason = env->NewStringUTF("ASMA.start");
    if (stop_reason == nullptr || start_reason == nullptr) {
      status = Unavailable("could not allocate ASMA surface lifecycle reason");
    }
  }
  if (status.ok()) {
    game_symbols_.pause_app(env, native_gl_class);
    status = CheckJni(env, "pause LuaApp for surface recreation");
  }
  if (status.ok()) {
    game_symbols_.set_task_scheduler_background_mode(env, native_gl_class,
                                                     JNI_TRUE, stop_reason);
    status = CheckJni(env, "background LuaApp for surface recreation");
  }
  if (status.ok()) {
    game_symbols_.set_task_scheduler_background_mode(env, native_gl_class,
                                                     JNI_FALSE, start_reason);
    status = CheckJni(env, "foreground LuaApp for surface recreation");
  }
  if (status.ok()) {
    game_symbols_.start_app(env, native_gl_class, start_app_params);
    status = CheckJni(env, "restart LuaApp with recreated surface");
  }

  if (start_reason != nullptr)
    env->DeleteLocalRef(start_reason);
  if (stop_reason != nullptr)
    env->DeleteLocalRef(stop_reason);
  if (start_app_params != nullptr)
    env->DeleteLocalRef(start_app_params);
  if (platform_params != nullptr)
    env->DeleteLocalRef(platform_params);
  if (surface_object != nullptr)
    env->DeleteLocalRef(surface_object);
  if (status.ok()) {
    std::fprintf(
        stderr,
        "  [surface] LuaApp JNI recreated generation=%llu pixels=%ux%u\n",
        static_cast<unsigned long long>(surface.generation), surface.width,
        surface.height);
  }
  return status;
}

Status RobloxExperienceComposition::RebindLuaAppSurface(
    const GameSurface& surface) {
  if (surface.generation == 0 || surface.native_window == 0 ||
      surface.width == 0 || surface.height == 0) {
    return Invalid("LuaApp surface rebind requires a complete surface");
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  jobject surface_object = nullptr;
  jobject platform_params = nullptr;
  status = BuildLaunchObjects(surface, &surface_object, &platform_params);
  if (!status.ok()) {
    return status;
  }

  jclass native_gl_class = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscribed_ || game_active_ || objects_ == nullptr ||
        objects_->native_gl_class == nullptr) {
      status = FailedPrecondition(
          "LuaApp surface rebind requires active LuaApp JNI roots");
    } else {
      native_gl_class = objects_->native_gl_class;
    }
  }
  if (status.ok()) {
    game_symbols_.update_surface_app(env, native_gl_class, surface_object,
                                     platform_params);
    status = CheckJni(env, "update LuaApp surface");
  }
  env->DeleteLocalRef(platform_params);
  env->DeleteLocalRef(surface_object);
  if (status.ok()) {
    std::fprintf(stderr,
                 "  [surface] LuaApp JNI rebind generation=%llu pixels=%ux%u\n",
                 static_cast<unsigned long long>(surface.generation),
                 surface.width, surface.height);
  }
  return status;
}

Status RobloxExperienceComposition::ReleaseGlobalObjects() {
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;
  std::unique_ptr<GlobalObjects> objects;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    objects = std::move(objects_);
    platform_protocols_initialized_ = false;
    readiness_ = {};
    active_game_canonical_json_.clear();
    presence_request_.reset();
    playing_presence_published_ = false;
    controlled_switch_waiting_for_return_ = false;
    lua_app_surface_recreation_pending_ = false;
    pending_launch_requests_.clear();
    next_request_id_ = 1;
  }
  if (objects == nullptr)
    return Status::Ok();
  if (objects->message_bus != nullptr)
    env->DeleteGlobalRef(objects->message_bus);
  if (objects->activity != nullptr)
    env->DeleteGlobalRef(objects->activity);
  if (objects->native_gl_class != nullptr)
    env->DeleteGlobalRef(objects->native_gl_class);
  return CheckJni(env, "release experience composition JNI roots");
}

Status RobloxExperienceComposition::ReleaseLuaAppGlobalObjects() {
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;

  jclass native_gl_class = nullptr;
  jobject activity = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (objects_ != nullptr) {
      native_gl_class = objects_->native_gl_class;
      activity = objects_->activity;
      objects_->native_gl_class = nullptr;
      objects_->activity = nullptr;
    }
    readiness_ = {};
    active_game_canonical_json_.clear();
    presence_request_.reset();
    playing_presence_published_ = false;
    controlled_switch_waiting_for_return_ = false;
    pending_launch_requests_.clear();
    next_request_id_ = 1;
  }
  if (activity != nullptr)
    env->DeleteGlobalRef(activity);
  if (native_gl_class != nullptr)
    env->DeleteGlobalRef(native_gl_class);
  return CheckJni(env, "release LuaApp experience JNI roots");
}

}  // namespace runtime
}  // namespace mocktail
