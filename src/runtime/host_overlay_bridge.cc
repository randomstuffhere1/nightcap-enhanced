#include "mocktail/graphics/text_overlay_frame.h"

#include "runtime/host_overlay_bridge.h"
#include "runtime/server_info_overlay.h"

namespace mocktail::runtime {

void AnchorHostOverlayProvider() {}

}  // namespace mocktail::runtime

// The host's own layer of the overlay compositor.
//
// The compositor draws two layers: the text surface the client draws its chat
// field into, and this one, which the host owns. The server info overlay
// publishes its popup, and its frame rate and ping read-out, here, so a notice
// can be on screen while the client's own text is up: the two layers are drawn
// one after the other and a notice is never hidden behind a chat box.
//
// The two layers carry the same layout, so the frame is copied across field by
// field. Only the ABI version differs, because each layer has its own.
extern "C" bool mocktail_host_overlay_query(
    MocktailHostOverlayFrameInfo* frame) {
  if (frame == nullptr) {
    return false;
  }
  MocktailTextOverlayFrameInfo source{};
  if (!mocktail::runtime::ServerInfoOverlay::QueryFrame(&source)) {
    return false;
  }
  frame->abi_version = MocktailHostOverlayFrameInfo::kAbiVersion;
  frame->visible = source.visible;
  frame->revision = source.revision;
  frame->coordinate_width = source.coordinate_width;
  frame->coordinate_height = source.coordinate_height;
  frame->x = source.x;
  frame->y = source.y;
  frame->width = source.width;
  frame->height = source.height;
  frame->row_bytes = source.row_bytes;
  frame->rgba_bytes = source.rgba_bytes;
  return true;
}

extern "C" bool mocktail_host_overlay_copy(std::uint64_t revision, void* rgba,
                                           std::size_t rgba_capacity) {
  return mocktail::runtime::ServerInfoOverlay::CopyFrame(revision, rgba,
                                                        rgba_capacity);
}
