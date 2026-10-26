# Modified by vii from komaruworld/mocktail. See README "About this fork".
.DEFAULT_GOAL := all
.PHONY: all build release debug test apk install register-url-handler release-runtime portable portable-test standalone appimage flatpak flatpak-build run-flatpak run run-smoke run-unlimited run-game run-input run-resize run-network auto-run run-gles run-angle update-roblox update-auto update-auto-launch payload-status payload-rollback support-bundle clean submodules help

BUILD_DIR  := build
TEST_BUILD_DIR ?= build-test
BINARY     := $(BUILD_DIR)/mocktail
JOBS       := $(shell nproc)
BUILD_TYPE ?= Release
LIBC ?= auto
MODE ?= standalone
CMAKE_TOOLCHAIN_FILE ?=
CMAKE_SYSROOT ?=
CMAKE_GENERATOR ?= auto
CMAKE_EXTRA_ARGS ?=
# Enable via `make build MOCKTAIL_ENABLE_UNITY_BUILD=ON MOCKTAIL_ENABLE_PCH=ON` for max clean-build speed.
MOCKTAIL_ENABLE_UNITY_BUILD ?=
MOCKTAIL_ENABLE_PCH ?=
MOCKTAIL_DISABLE_CCACHE ?=
MOCKTAIL_DISABLE_FAST_LINKER ?=
RELEASE_BUILD_DIR ?=
PORTABLE_MODE ?= standalone
PORTABLE_CANONICAL_MODE := $(if $(filter dynamic minimal,$(PORTABLE_MODE)),thin,$(if $(filter full static,$(PORTABLE_MODE)),standalone,$(PORTABLE_MODE)))
PORTABLE_DIR ?= dist/mocktail-linux-x86_64-glibc-$(PORTABLE_CANONICAL_MODE)
APPIMAGE ?= dist/Mocktail-x86_64.AppImage
APPIMAGE_FORMAT ?= classic
ANYLINUX_PACKAGER ?= quick-sharun
ANYLINUX_APPIMAGETOOL ?= appimagetool
PREFIX ?= /usr
FLATPAK_BUILD_DIR ?= build-flatpak
FLATPAK_MANIFEST ?= packaging/flatpak/io.github.CoderDayton.nightcap.json
FLATPAK_JOBS ?= 2

# Auto-detect fastest generator: Ninja > Unix Makefiles.  Override with
# `make build CMAKE_GENERATOR="Unix Makefiles"` if needed.
ifeq ($(CMAKE_GENERATOR),auto)
  _MOCKTAIL_HAS_NINJA := $(shell command -v ninja >/dev/null 2>&1 && echo 1 || echo 0)
  MOCKTAIL_CMAKE_GENERATOR := $(if $(filter 1,$(_MOCKTAIL_HAS_NINJA)),Ninja,Unix Makefiles)
else
  MOCKTAIL_CMAKE_GENERATOR := $(CMAKE_GENERATOR)
endif

define build_native_runtime
	@if ! git submodule status --recursive 2>/dev/null | grep -q "^-"; then \
		echo "[mocktail] submodules up to date"; \
	else \
		echo "[mocktail] initializing submodules..."; \
		git submodule update --init --recursive --jobs "$(JOBS)"; \
	fi
	@_need_configure=0; _need_wipe=0; \
	if [ ! -f "$(3)/CMakeCache.txt" ]; then \
		_need_configure=1; \
	elif ! grep -q "CMAKE_GENERATOR:INTERNAL=$(MOCKTAIL_CMAKE_GENERATOR)" "$(3)/CMakeCache.txt" 2>/dev/null; then \
		echo "[mocktail] generator changed -> $(MOCKTAIL_CMAKE_GENERATOR), wiping cache..."; \
		_need_configure=1; _need_wipe=1; \
	elif ! grep -q "CMAKE_BUILD_TYPE:STRING=$(1)" "$(3)/CMakeCache.txt" 2>/dev/null; then \
		_need_configure=1; \
	elif ! grep -q "BUILD_TESTING:BOOL=$(2)" "$(3)/CMakeCache.txt" 2>/dev/null; then \
		_need_configure=1; \
	fi; \
	if [ "$$_need_wipe" = "1" ]; then rm -rf "$(3)/CMakeCache.txt" "$(3)/CMakeFiles"; fi; \
	if [ "$$_need_configure" = "1" ]; then \
		echo "[mocktail] configuring ($(MOCKTAIL_CMAKE_GENERATOR), $(1), testing=$(2))..."; \
		cmake -S . -B "$(3)" -G "$(MOCKTAIL_CMAKE_GENERATOR)" \
			-DCMAKE_BUILD_TYPE="$(1)" \
			-DBUILD_TESTING="$(2)" \
			-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
			$(if $(strip $(MOCKTAIL_ENABLE_UNITY_BUILD)),-DMOCKTAIL_ENABLE_UNITY_BUILD="$(MOCKTAIL_ENABLE_UNITY_BUILD)") \
			$(if $(strip $(MOCKTAIL_ENABLE_PCH)),-DMOCKTAIL_ENABLE_PCH="$(MOCKTAIL_ENABLE_PCH)") \
			$(if $(strip $(MOCKTAIL_DISABLE_CCACHE)),-DMOCKTAIL_DISABLE_CCACHE="$(MOCKTAIL_DISABLE_CCACHE)") \
			$(if $(strip $(MOCKTAIL_DISABLE_FAST_LINKER)),-DMOCKTAIL_DISABLE_FAST_LINKER="$(MOCKTAIL_DISABLE_FAST_LINKER)") \
			$(if $(strip $(CMAKE_TOOLCHAIN_FILE)),-DCMAKE_TOOLCHAIN_FILE="$(CMAKE_TOOLCHAIN_FILE)") \
			$(if $(strip $(CMAKE_SYSROOT)),-DCMAKE_SYSROOT="$(CMAKE_SYSROOT)") \
			$(CMAKE_EXTRA_ARGS); \
	else \
		echo "[mocktail] build dir already configured, skipping cmake configure"; \
	fi
	@cmake --build "$(3)" -j"$(JOBS)"
endef

all: build ## Dynamic Release build

build: ## Build the dynamic Release runtime in build/
	$(call build_native_runtime,$(BUILD_TYPE),OFF,$(BUILD_DIR))

release: ## Build AppImage (LIBC=auto|glibc|musl MODE=standalone|thin)
	@MOCKTAIL_BUILD_JOBS="$(JOBS)" \
		MOCKTAIL_BUILD_TYPE="$(BUILD_TYPE)" \
		MOCKTAIL_RELEASE_BUILD_DIR="$(RELEASE_BUILD_DIR)" \
		CMAKE_TOOLCHAIN_FILE="$(CMAKE_TOOLCHAIN_FILE)" \
		CMAKE_SYSROOT="$(CMAKE_SYSROOT)" \
		CMAKE_GENERATOR="$(CMAKE_GENERATOR)" \
		CMAKE_EXTRA_ARGS="$(CMAKE_EXTRA_ARGS)" \
		MOCKTAIL_ENABLE_UNITY_BUILD="$(MOCKTAIL_ENABLE_UNITY_BUILD)" \
		MOCKTAIL_ENABLE_PCH="$(MOCKTAIL_ENABLE_PCH)" \
		MOCKTAIL_DISABLE_CCACHE="$(MOCKTAIL_DISABLE_CCACHE)" \
		MOCKTAIL_DISABLE_FAST_LINKER="$(MOCKTAIL_DISABLE_FAST_LINKER)" \
		./scripts/build_release.sh --libc "$(LIBC)" --mode "$(MODE)"

debug: ## Debug build
	$(call build_native_runtime,Debug,OFF,$(BUILD_DIR))

install: ## Install an already-built dynamic runtime (use: sudo make install)
	@cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

register-url-handler: build ## Select this build for Roblox website links
	@./scripts/register_url_handler.sh --set-default \
		--executable "$(abspath $(BINARY))" \
		--desktop-file "$(abspath packaging/io.github.CoderDayton.nightcap.desktop)"

test: ## Build and run all unit tests in build-test/
	$(call build_native_runtime,Debug,ON,$(TEST_BUILD_DIR))
	@ctest --test-dir "$(TEST_BUILD_DIR)" --output-on-failure -j"$(JOBS)"

apk: ## Extract libroblox.so and build (usage: make apk APK=/path/to/roblox.apk)
ifndef APK
	$(error APK variable not set. Usage: make apk APK=/path/to/roblox-x86_64.apk)
endif
	@./scripts/build.sh --apk "$(APK)" --build-type $(BUILD_TYPE) --jobs $(JOBS)

release-runtime: build

portable: appimage ## Build the portable AppImage

portable-test: release-runtime ## Verify portable packaging and relocation
	@./tests/portable_packaging_test.sh

standalone: appimage ## Build the standalone x86-64 AppImage

appimage: release-runtime ## Build AppImage (APPIMAGE_FORMAT=classic|anylinux)
	@MOCKTAIL_APPIMAGE_FORMAT="$(APPIMAGE_FORMAT)" \
		MOCKTAIL_ANYLINUX_PACKAGER="$(ANYLINUX_PACKAGER)" \
		MOCKTAIL_ANYLINUX_APPIMAGETOOL="$(ANYLINUX_APPIMAGETOOL)" \
		./scripts/package_portable.sh --build-dir "$(BUILD_DIR)" \
		--libc glibc --output "$(PORTABLE_DIR)" --mode "$(PORTABLE_MODE)" \
		--appimage "$(APPIMAGE)"

flatpak: ## Build and install the local x86-64 Flatpak
	@./scripts/build_flatpak.sh \
		--build-dir "$(FLATPAK_BUILD_DIR)" \
		--manifest "$(FLATPAK_MANIFEST)" \
		--jobs "$(FLATPAK_JOBS)"

flatpak-build: ## Build the local Flatpak without installing it
	@./scripts/build_flatpak.sh \
		--no-install \
		--build-dir "$(FLATPAK_BUILD_DIR)" \
		--manifest "$(FLATPAK_MANIFEST)" \
		--jobs "$(FLATPAK_JOBS)"

run-flatpak: ## Run the installed Flatpak
	@flatpak run io.github.CoderDayton.nightcap

run: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 0
run: build ## Run the native binary interactively until the window is closed
	@$(BINARY)

run-smoke: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 3000
run-smoke: release-runtime ## Run the short Vulkan lifecycle readiness gate
	@./scripts/real_bringup_smoke.sh C

startup-timeline: release-runtime ## Time startup per phase and print a median timeline (opens a window)
	@./scripts/startup_timeline.sh

run-unlimited: export MOCKTAIL_FRAME_RATE_LIMIT = unlimited
run-unlimited: export MOCKTAIL_VSYNC = off
run-unlimited: export MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS = 5000
run-unlimited: release-runtime ## Run 240-FPS/unthrottled Vulkan diagnostic mode
	@./scripts/real_bringup_smoke.sh C

run-game: release-runtime ## Run local UGCGame/Vulkan gate (not an authenticated join)
	@./scripts/real_bringup_smoke.sh GAME

run-input: release-runtime ## Run local GAME gate and route SDL mouse/touch through native input
	@./scripts/real_bringup_smoke.sh INPUT

run-resize: release-runtime ## Run real SDL compositor resize and typed JNI surface-rebind gate
	@./scripts/real_bringup_smoke.sh RESIZE

run-network: export MOCKTAIL_PLACE_ID = $(PLACE_ID)
run-network: release-runtime ## Run authenticated public-place gate (usage: make run-network PLACE_ID=...)
ifndef PLACE_ID
	$(error PLACE_ID variable not set. Usage: make run-network PLACE_ID=positive-place-id)
endif
	@./scripts/real_bringup_smoke.sh NETWORK

auto-run: ## Build/run loop with crash summaries
	@./scripts/auto_runtime_loop.sh

run-gles: build ## Run strict system EGL/OpenGL ES 3 without Vulkan fallback
	@$(BINARY) --graphics opengl

run-angle: build ## Run with ANGLE/Vulkan compatibility backend
	@$(BINARY) --graphics angle-vulkan

update-roblox: ## Validate and import the current x86_64 Roblox bundle cached by Sober
	@./scripts/update_roblox_payload.sh

update-auto: build ## Download latest Roblox, derive its ABI, run two canaries, then activate
	@./$(BUILD_DIR)/mocktail_updater update

update-auto-launch: build ## Update Roblox and launch the verified current payload
	@./$(BUILD_DIR)/mocktail_updater update
	@./$(BINARY)

payload-status: build ## Show current and previous-good immutable payloads
	@./$(BUILD_DIR)/mocktail_updater status

payload-rollback: build ## Atomically restore the previous-good payload
	@./$(BUILD_DIR)/mocktail_updater rollback

support-bundle: ## Create a private sanitized diagnostics archive
	@./scripts/collect_support_bundle.sh --context manual --reason manual-request

submodules: ## Initialise git submodules (libjnivm, Vulkan-Headers)
	@./scripts/add_submodules.sh

clean: ## Remove the build and test build directories
	rm -rf $(BUILD_DIR) $(TEST_BUILD_DIR)
	@echo "Build directories removed."

help: ## Show available targets
	@grep -E '^[a-zA-Z_-]+:.*##' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*## "}; {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}'
