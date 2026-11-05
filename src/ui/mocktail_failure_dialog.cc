// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include <adwaita.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(MOCKTAIL_HAVE_X11)
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

#include "mocktail/platform/application_id.h"

namespace {

constexpr char kMessagePacket = 'M';
constexpr char kFailurePacket = 'F';
constexpr char kSuccessPacket = 'S';
constexpr char kProgressPacket = 'P';
constexpr char kConfirmationReadyPacket = 'R';
constexpr char kConfirmationErrorPacket = 'E';
constexpr int kConfirmationReadyFd = 3;
constexpr char kLogFolderPacket = 'L';
constexpr std::size_t kMaximumPacketBytes = 4096;
constexpr std::string_view kDefaultMessage =
    "Nightcap closed unexpectedly because of an internal error.";
constexpr std::string_view kDefaultProgressMessage = "Installing Roblox...";
constexpr std::string_view kConfirmationUnavailableMessage =
    "Roblox could not verify this game. The private server was not joined.";
// Keep AdwAlertDialog semantics while preventing host GTK themes such as
// Breeze from replacing the compact dark crash-card appearance.
constexpr char kDialogStyle[] = R"css(
dialog.alert,
window.dialog-window.alert {
  background-color: #38393e;
  color: #ffffff;
  border-radius: 18px;
  border: none;
  outline: none;
  box-shadow: none;
}

.alert .message-area {
  padding-top: 32px;
  padding-bottom: 9px;
  border-spacing: 24px;
}

.alert .message-area.has-heading.has-body {
  border-spacing: 10px;
}

.alert .message-area > .heading-bin,
.alert .message-area > .body,
.alert .message-area > .child {
  padding-left: 24px;
  padding-right: 24px;
}

.alert .heading-bin > label {
  color: #ffffff;
  font-size: 18px;
  font-weight: 700;
}

.alert .body {
  color: rgba(255, 255, 255, 0.92);
  font-size: 14px;
}

.alert .response-area {
  padding: 24px;
  padding-top: 12px;
  border-spacing: 12px;
}

.alert .response-area > button {
  min-height: 24px;
  min-width: 60px;
  padding: 10px 20px;
  border: none;
  border-radius: 12px;
  color: #ffffff;
  background: #515257;
  box-shadow: none;
}

.alert .response-area > button:hover {
  background: #5c5d62;
}

.alert .response-area > button:active {
  background: #48494e;
}

.alert .response-area > button:disabled {
  color: rgba(255, 255, 255, 0.42);
  background: #3d3e42;
}
)css";

constexpr char kProgressStyle[] = R"css(
window.mocktail-update-progress {
  background-color: #202125;
  color: #f7f7f8;
}

window.mocktail-update-progress headerbar {
  background: transparent;
  border: none;
  box-shadow: none;
}

.mocktail-update-progress-content {
  border-spacing: 34px;
}

.mocktail-update-progress-label {
  color: #f7f7f8;
  font-size: 26px;
  font-weight: 700;
}

.mocktail-update-progress-spinner {
  min-width: 64px;
  min-height: 64px;
  -gtk-icon-size: 64px;
  color: #f7f7f8;
}
)css";

struct ProgressState {
  GMainLoop* loop = nullptr;
  GtkWindow* window = nullptr;
  GtkLabel* label = nullptr;
  guint source_id = 0;
};

enum class UiStyle {
  kDialog,
  kProgress,
};


std::string ValidMessage(std::string_view message) {
  if (message.empty() ||
      !g_utf8_validate(message.data(), static_cast<gssize>(message.size()),
                       nullptr)) {
    return std::string(kDefaultMessage);
  }
  return std::string(message);
}

void InstallDialogStyle(GdkDisplay* display) {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kDialogStyle);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_object_unref(provider);
}

void InstallProgressStyle(GdkDisplay* display) {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kProgressStyle);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_object_unref(provider);
}

bool InitializeUi(UiStyle style) {
  g_set_application_name("Nightcap");
  // GTK names a window's app after the program. Without this, docks cannot
  // tie the window to Nightcap's desktop entry and give it an icon of its own.
  g_set_prgname(mocktail::platform::kApplicationId);
  if (!gtk_init_check()) {
    return false;
  }
  // Libadwaita owns the color scheme below; clear the unsupported legacy
  // preference before initialization so KDE settings do not emit a warning.
  GtkSettings* gtk_settings = gtk_settings_get_default();
  if (gtk_settings != nullptr) {
    g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", FALSE,
                 nullptr);
  }
  adw_init();
  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    return false;
  }
  gtk_window_set_default_icon_name(mocktail::platform::kApplicationId);
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                     ADW_COLOR_SCHEME_FORCE_DARK);
  adw_style_manager_set_color_scheme(adw_style_manager_get_for_display(display),
                                     ADW_COLOR_SCHEME_FORCE_DARK);
  if (style == UiStyle::kDialog) {
    InstallDialogStyle(display);
  } else if (style == UiStyle::kProgress) {
    InstallProgressStyle(display);
  } else {
  }
  return true;
}

struct DialogState {
  GMainLoop* loop = nullptr;
  std::string log_folder;
  bool launching = false;
  bool closed = false;
};

void OnDialogClosed(AdwDialog*, gpointer user_data) {
  auto* state = static_cast<DialogState*>(user_data);
  state->closed = true;
  if (!state->launching) g_main_loop_quit(state->loop);
}

void OnLogFolderLaunched(GObject* source, GAsyncResult* result,
                         gpointer user_data) {
  auto* state = static_cast<DialogState*>(user_data);
  GError* error = nullptr;
  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result,
                                       &error)) {
    g_printerr("[dialog] cannot open %s: %s\n", state->log_folder.c_str(),
               error != nullptr ? error->message : "unknown error");
    g_clear_error(&error);
  }
  state->launching = false;
  if (state->closed) g_main_loop_quit(state->loop);
}

void OnDialogResponse(AdwAlertDialog*, const char* response,
                      gpointer user_data) {
  auto* state = static_cast<DialogState*>(user_data);
  if (std::string_view(response) != "logs") return;
  GFile* folder = g_file_new_for_path(state->log_folder.c_str());
  GtkFileLauncher* launcher = gtk_file_launcher_new(folder);
  state->launching = true;
  gtk_file_launcher_launch(launcher, nullptr, nullptr, OnLogFolderLaunched,
                           state);
  g_object_unref(launcher);
  g_object_unref(folder);
}

struct ConfirmationState {
  bool confirmed = false;
  bool ready = false;
  bool closed = false;
  guint ready_source_id = 0;
  GMainLoop* loop = nullptr;
  AdwAlertDialog* alert = nullptr;
};

std::string ValidConfirmationMessage(std::string_view message) {
  if (message.empty() ||
      !g_utf8_validate(message.data(), static_cast<gssize>(message.size()),
                       nullptr)) {
    return std::string(kConfirmationUnavailableMessage);
  }
  return std::string(message);
}

void SetConfirmationBody(ConfirmationState* state, std::string_view message) {
  if (state == nullptr || state->alert == nullptr) return;
  const std::string body = ValidConfirmationMessage(message);
  adw_alert_dialog_set_body(state->alert, body.c_str());
}

void CenterStandaloneDialog(AdwDialog* dialog);
gboolean CenterStandaloneDialogIdle(gpointer data);

gboolean OnConfirmationReadyPacket(gint descriptor, GIOCondition condition,
                                  gpointer user_data) {
  auto* state = static_cast<ConfirmationState*>(user_data);
  if (state == nullptr || state->closed) return G_SOURCE_REMOVE;

  char packet[kMaximumPacketBytes];
  ssize_t received = 0;
  if ((condition & G_IO_IN) != 0) {
    do {
      received = recv(descriptor, packet, sizeof(packet), 0);
    } while (received < 0 && errno == EINTR);
  }
  if (received <= 0) {
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
      state->ready_source_id = 0;
      SetConfirmationBody(state, kConfirmationUnavailableMessage);
      return G_SOURCE_REMOVE;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return G_SOURCE_CONTINUE;
    }
    state->ready_source_id = 0;
    SetConfirmationBody(state, kConfirmationUnavailableMessage);
    return G_SOURCE_REMOVE;
  }
  if (static_cast<std::size_t>(received) > sizeof(packet) ||
      (packet[0] != kConfirmationReadyPacket &&
       packet[0] != kConfirmationErrorPacket)) {
    state->ready_source_id = 0;
    SetConfirmationBody(state, kConfirmationUnavailableMessage);
    return G_SOURCE_REMOVE;
  }

  const std::string_view message(packet + 1,
                                 static_cast<std::size_t>(received - 1));
  if (packet[0] == kConfirmationReadyPacket) {
    SetConfirmationBody(state, message);
    adw_alert_dialog_set_response_enabled(state->alert, "yes", TRUE);
    adw_alert_dialog_set_default_response(state->alert, "yes");
    state->ready = true;
    CenterStandaloneDialog(ADW_DIALOG(state->alert));
    g_timeout_add(1, CenterStandaloneDialogIdle,
                  g_object_ref(ADW_DIALOG(state->alert)));
  } else {
    SetConfirmationBody(state, message);
    CenterStandaloneDialog(ADW_DIALOG(state->alert));
    g_timeout_add(1, CenterStandaloneDialogIdle,
                  g_object_ref(ADW_DIALOG(state->alert)));
  }
  state->ready_source_id = 0;
  return G_SOURCE_REMOVE;
}

void OnConfirmationClosed(AdwDialog*, gpointer user_data) {
  auto* state = static_cast<ConfirmationState*>(user_data);
  if (state == nullptr) return;
  state->closed = true;
  if (state->ready_source_id != 0) {
    g_source_remove(state->ready_source_id);
    state->ready_source_id = 0;
  }
  g_main_loop_quit(state->loop);
}

void OnConfirmationResponse(AdwDialog*, gchar* response, gpointer user_data) {
  auto* state = static_cast<ConfirmationState*>(user_data);
  if (state != nullptr) {
    state->confirmed = state->ready && response != nullptr &&
                       std::string_view(response) == "yes";
  }
}

GtkWidget* CreateDialogParent() {
  // AdwDialog creates a non-resizable top-level window when its parent is a
  // plain GtkWindow. A transparent full-screen parent gives that window a
  // transient parent for window-manager centering without adding a backdrop.
  GtkWidget* parent = gtk_window_new();
  g_object_ref_sink(parent);
  GtkWindow* window = GTK_WINDOW(parent);
  gtk_window_set_title(window, "Nightcap");
  gtk_window_set_decorated(window, FALSE);
  gtk_window_set_resizable(window, FALSE);
  gtk_window_set_modal(window, TRUE);
  gtk_widget_set_opacity(parent, 0.0);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(content, 1, 1);
  gtk_window_set_child(window, content);
  gtk_window_fullscreen(window);
  gtk_window_present(window);
  return parent;
}

void DestroyDialogParent(GtkWidget* parent) {
  if (parent == nullptr) return;
  gtk_window_destroy(GTK_WINDOW(parent));
  g_object_unref(parent);
}

void PresentDialog(AdwDialog* dialog, GtkWidget* parent) {
  adw_dialog_set_presentation_mode(dialog, ADW_DIALOG_FLOATING);
  adw_dialog_present(dialog, parent);
}

void CenterStandaloneDialog(AdwDialog* dialog) {
  if (dialog == nullptr) return;
  GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(dialog));
  if (!GTK_IS_WINDOW(root)) return;
  GtkWindow* window = GTK_WINDOW(root);
  // AdwDialog already uses a non-resizable window, but enforce it after
  // presentation as well so WM move/resize gestures cannot shrink its content.
  gtk_window_set_resizable(window, FALSE);

#if defined(MOCKTAIL_HAVE_X11)
  GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window));
  if (display == nullptr || !GDK_IS_X11_DISPLAY(display)) return;
  GtkNative* native = gtk_widget_get_native(GTK_WIDGET(window));
  if (native == nullptr) return;
  GdkSurface* surface = gtk_native_get_surface(native);
  if (surface == nullptr || !GDK_IS_X11_SURFACE(surface)) return;
  Display* xdisplay = GDK_DISPLAY_XDISPLAY(display);
  if (xdisplay == nullptr) return;

  XWindowAttributes attributes{};
  if (!XGetWindowAttributes(xdisplay, GDK_SURFACE_XID(surface), &attributes) ||
      attributes.width <= 0 || attributes.height <= 0) {
    return;
  }

  GdkRectangle bounds{};
  GdkMonitor* monitor = gdk_display_get_monitor_at_surface(display, surface);
  if (monitor != nullptr) {
    gdk_monitor_get_geometry(monitor, &bounds);
  }
  if (bounds.width <= 0 || bounds.height <= 0) {
    const int screen = DefaultScreen(xdisplay);
    bounds.x = 0;
    bounds.y = 0;
    bounds.width = DisplayWidth(xdisplay, screen);
    bounds.height = DisplayHeight(xdisplay, screen);
  }
  const int x = bounds.x + (bounds.width - attributes.width) / 2;
  const int y = bounds.y + (bounds.height - attributes.height) / 2;
  XMoveWindow(xdisplay, GDK_SURFACE_XID(surface), x, y);
  XFlush(xdisplay);
#endif
}

gboolean CenterStandaloneDialogIdle(gpointer data) {
  CenterStandaloneDialog(static_cast<AdwDialog*>(data));
  g_object_unref(data);
  return G_SOURCE_REMOVE;
}

int ShowConfirmation(std::string_view requested_message,
                     bool wait_for_ready) {
  if (!InitializeUi(UiStyle::kDialog)) {
    return EXIT_FAILURE;
  }

  const std::string message = ValidMessage(requested_message);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  AdwDialog* dialog =
      adw_alert_dialog_new("Join private server?", message.c_str());
  g_object_ref_sink(dialog);
  adw_dialog_set_content_width(dialog, 370);
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  ConfirmationState state;
  state.loop = loop;
  state.alert = alert;
  state.ready = !wait_for_ready;
  adw_alert_dialog_add_response(alert, "no", "No");
  adw_alert_dialog_add_response(alert, "yes", "Yes");
  adw_alert_dialog_set_response_enabled(alert, "yes", !wait_for_ready);
  adw_alert_dialog_set_response_appearance(alert, "yes",
                                          ADW_RESPONSE_SUGGESTED);
  // Window-manager close and Escape are cancellation, never approval.
  adw_alert_dialog_set_close_response(alert, "no");
  adw_alert_dialog_set_default_response(alert, "no");
  g_signal_connect(dialog, "response", G_CALLBACK(OnConfirmationResponse),
                   &state);
  g_signal_connect(dialog, "closed", G_CALLBACK(OnConfirmationClosed), &state);
  if (wait_for_ready) {
    state.ready_source_id = g_unix_fd_add(
        kConfirmationReadyFd,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        OnConfirmationReadyPacket, &state);
    if (state.ready_source_id == 0) {
      g_object_unref(dialog);
      g_main_loop_unref(loop);
      return EXIT_FAILURE;
    }
  }
  GtkWidget* parent = CreateDialogParent();
  PresentDialog(dialog, parent);
  CenterStandaloneDialog(dialog);
  g_idle_add(CenterStandaloneDialogIdle, g_object_ref(dialog));
  g_main_loop_run(loop);
  if (state.ready_source_id != 0) {
    g_source_remove(state.ready_source_id);
    state.ready_source_id = 0;
  }
  DestroyDialogParent(parent);
  g_object_unref(dialog);
  g_main_loop_unref(loop);
  return state.confirmed ? EXIT_SUCCESS : 2;
}

int ShowDialog(std::string_view requested_message,
               const char* heading = "Nightcap",
               const char* response_label = "Close",
               std::string_view log_folder = {}) {
  if (!InitializeUi(UiStyle::kDialog)) {
    return EXIT_FAILURE;
  }

  const std::string message = ValidMessage(requested_message);
  DialogState state;
  state.loop = g_main_loop_new(nullptr, FALSE);
  state.log_folder = std::string(log_folder);
  AdwDialog* dialog = adw_alert_dialog_new(heading, message.c_str());
  g_object_ref_sink(dialog);
  adw_dialog_set_content_width(dialog, 420);
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  // The narrow layout caps the width near 370 px and ignores the setting.
  adw_alert_dialog_set_prefer_wide_layout(alert, TRUE);
  if (!state.log_folder.empty() &&
      g_utf8_validate(state.log_folder.c_str(), -1, nullptr)) {
    adw_alert_dialog_add_response(alert, "logs", "Open Log Folder");
  }
  adw_alert_dialog_add_response(alert, "close", response_label);
  adw_alert_dialog_set_close_response(alert, "close");
  adw_alert_dialog_set_default_response(alert, "close");
  g_signal_connect(dialog, "response", G_CALLBACK(OnDialogResponse), &state);
  g_signal_connect(dialog, "closed", G_CALLBACK(OnDialogClosed), &state);
  PresentDialog(dialog, nullptr);
  g_main_loop_run(state.loop);
  g_object_unref(dialog);
  g_main_loop_unref(state.loop);
  return EXIT_SUCCESS;
}

std::string ValidProgressMessage(std::string_view message) {
  if (message.empty() ||
      !g_utf8_validate(message.data(), static_cast<gssize>(message.size()),
                       nullptr)) {
    return std::string(kDefaultProgressMessage);
  }
  return std::string(message);
}


gboolean OnProgressWindowClose(GtkWindow*, gpointer user_data) {
  auto* state = static_cast<ProgressState*>(user_data);
  g_main_loop_quit(state->loop);
  return FALSE;
}

void PresentProgressWindow(ProgressState* state, std::string_view message) {
  GtkWidget* window_widget = adw_window_new();
  state->window = GTK_WINDOW(g_object_ref_sink(window_widget));
  gtk_widget_add_css_class(window_widget, "mocktail-update-progress");
  gtk_window_set_title(state->window, "Nightcap");
  gtk_window_set_default_size(state->window, 470, 640);

  GtkWidget* toolbar_widget = adw_toolbar_view_new();
  auto* toolbar = ADW_TOOLBAR_VIEW(toolbar_widget);
  GtkWidget* header_widget = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header_widget), FALSE);
  adw_toolbar_view_add_top_bar(toolbar, header_widget);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 34);
  gtk_widget_add_css_class(content, "mocktail-update-progress-content");
  gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(content, TRUE);
  gtk_widget_set_vexpand(content, TRUE);

  const std::string valid_message = ValidProgressMessage(message);
  GtkWidget* label_widget = gtk_label_new(valid_message.c_str());
  state->label = GTK_LABEL(label_widget);
  gtk_widget_add_css_class(label_widget, "mocktail-update-progress-label");
  gtk_label_set_justify(state->label, GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap(state->label, TRUE);
  gtk_label_set_max_width_chars(state->label, 32);
  gtk_box_append(GTK_BOX(content), label_widget);

  GtkWidget* spinner = adw_spinner_new();
  gtk_widget_add_css_class(spinner, "mocktail-update-progress-spinner");
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(spinner, 64, 64);
  gtk_box_append(GTK_BOX(content), spinner);

  adw_toolbar_view_set_content(toolbar, content);
  adw_window_set_content(ADW_WINDOW(window_widget), toolbar_widget);
  g_signal_connect(state->window, "close-request",
                   G_CALLBACK(OnProgressWindowClose), state);
  gtk_window_present(state->window);
}

gboolean OnProgressPacket(gint descriptor, GIOCondition condition,
                          gpointer user_data) {
  auto* state = static_cast<ProgressState*>(user_data);
  if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
    state->source_id = 0;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }

  char packet[kMaximumPacketBytes];
  const ssize_t received = recv(descriptor, packet, sizeof(packet), 0);
  if (received < 0 && (errno == EINTR || errno == EAGAIN)) {
    return G_SOURCE_CONTINUE;
  }
  if (received <= 0 || packet[0] == kSuccessPacket ||
      packet[0] == kFailurePacket) {
    state->source_id = 0;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }
  if (packet[0] != kProgressPacket) {
    return G_SOURCE_CONTINUE;
  }

  const std::string message = ValidProgressMessage(
      std::string_view(packet + 1, static_cast<std::size_t>(received - 1)));
  if (state->window == nullptr) {
    PresentProgressWindow(state, message);
  } else {
    gtk_label_set_text(state->label, message.c_str());
  }
  return G_SOURCE_CONTINUE;
}

int MonitorProgress() {
  if (!InitializeUi(UiStyle::kProgress)) {
    return EXIT_FAILURE;
  }
  ProgressState state;
  state.loop = g_main_loop_new(nullptr, FALSE);
  state.source_id = g_unix_fd_add(
      STDIN_FILENO,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
      OnProgressPacket, &state);
  if (state.source_id == 0) {
    g_main_loop_unref(state.loop);
    return EXIT_FAILURE;
  }
  g_main_loop_run(state.loop);
  if (state.source_id != 0) {
    g_source_remove(state.source_id);
  }
  if (state.window != nullptr) {
    gtk_window_destroy(state.window);
    g_object_unref(state.window);
  }
  g_main_loop_unref(state.loop);
  return EXIT_SUCCESS;
}

int MonitorParent() {
  std::string message(kDefaultMessage);
  std::string log_folder;
  char packet[kMaximumPacketBytes];
  while (true) {
    const ssize_t received = recv(STDIN_FILENO, packet, sizeof(packet), 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return ShowDialog(message, "Nightcap", "Close", log_folder);
    }
    const std::string_view payload(packet + 1,
                                   static_cast<std::size_t>(received - 1));
    switch (packet[0]) {
      case kMessagePacket:
        message = ValidMessage(payload);
        break;
      case kLogFolderPacket:
        log_folder.assign(payload);
        break;
      case kFailurePacket:
        return ShowDialog(message, "Nightcap", "Close", log_folder);
      case kSuccessPacket:
        return EXIT_SUCCESS;
      default:
        break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--monitor") {
    return MonitorParent();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--progress-monitor") {
    return MonitorProgress();
  }
  if (argc == 3 && std::string_view(argv[1]) == "--message") {
    return ShowDialog(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--warning") {
    return ShowDialog(argv[2], "Signed out", "Continue");
  }
  if (argc == 3 && std::string_view(argv[1]) == "--confirm") {
    return ShowConfirmation(argv[2], false);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--confirm-ready") {
    return ShowConfirmation(argv[2], true);
  }
  return EXIT_FAILURE;
}
