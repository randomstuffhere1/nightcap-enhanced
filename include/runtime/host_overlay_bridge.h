#ifndef MOCKTAIL_RUNTIME_HOST_OVERLAY_BRIDGE_H_
#define MOCKTAIL_RUNTIME_HOST_OVERLAY_BRIDGE_H_

namespace mocktail::runtime {

// Keeps the host notice layer's provider in the binary.
//
// The compositor finds that provider by name, so the translation unit that
// defines it has to be linked in. It lives in a static library, and a static
// library drops the objects nothing refers to, so the runtime calls this once
// at startup.
void AnchorHostOverlayProvider();

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_HOST_OVERLAY_BRIDGE_H_
