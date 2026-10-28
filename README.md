<!-- Modified by vii from komaruworld/mocktail. See "About this fork" below. -->
<div align="center">
  <img src="https://raw.githubusercontent.com/CoderDayton/nightcap/main/packaging/io.github.CoderDayton.nightcap.svg" alt="Nightcap logo" width="128">
  <h1 style="margin-top: 5px;">
    Nightcap
    <br>
    <a href="https://github.com/CoderDayton/nightcap/actions/workflows/ci.yml"><img src="https://github.com/CoderDayton/nightcap/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
    <a href="https://github.com/CoderDayton/nightcap/stargazers"><img src="https://img.shields.io/github/stars/CoderDayton/nightcap?style=flat&logo=github" alt="Stars"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License"></a>
    <a href="https://discord.gg/DbAUG9hBFU"><img alt="Discord" src="https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fdiscord.com%2Fapi%2Fv10%2Finvites%2FDbAUG9hBFU%3Fwith_counts%3Dtrue&query=%24.approximate_member_count&label=Discord&logo=discord&logoColor=white&color=5865F2&suffix=%20members"></a>
  </h1>
</div>

Nightcap is Roblox on Linux, tuned for desktop play. It is a Mocktail fork
that keeps up with Roblox updates, with extra bug fixes, custom assets and
more. It runs the Android `x86_64` Roblox client on Linux, including a Linux
userspace hosted by FreeBSD's Linuxulator. It provides the Android ABI and JNI
pieces the client expects, then connects them to SDL3 and Vulkan or OpenGL on
the Linux side.

Nightcap is an independent community project. It is not affiliated with Roblox
Corporation or VinegarHQ and does not distribute the Roblox client.

## About this fork

Nightcap is a fork of [komaruworld/mocktail](https://github.com/komaruworld/mocktail)
with extra fixes for playing on a PC. The program and config paths keep the
`mocktail` name so upstream fixes still apply cleanly. The app ID is
`io.github.CoderDayton.nightcap`, so Nightcap and upstream can be installed
side by side.

- **Keeps up with Roblox.** New Roblox versions work soon after they come out.
- **Lower input lag.** Frames are shown as soon as they are ready instead of
  waiting in a queue.
- **Sharper textures.** Small textures are upscaled 4x by default, and you
  can replace any texture with your own PNG in `~/.config/mocktail/textures`.
  A ready-made crosshair is in [`textures/`](textures/README.md). Set
  `MOCKTAIL_SMALL_TEXTURE_UPSCALE=1` to turn the upscale off.
- **ETC2 textures on any GPU.** Games that use compressed mobile textures now
  render correctly on desktop GPUs, without stutter.
- **A pipeline cache that is already warm.** The client keeps its own
  `shadercachevk.bin` and hands it to the driver, so shaders are not
  recompiled on every launch. Nightcap also keeps a fallback copy for clients
  that do not do this; set `graphics.pipeline_cache: off` to turn it off.
- **Your own cursors, fonts and skies.** Drop files in
  `~/.local/share/mocktail/asset_overlay` and Nightcap uses them. The layout
  matches Sober's `asset_overlay` folder. See
  [`textures/`](textures/README.md) for which folder to use.
- **Plays more games.** Nightcap answers a server check that some experiences
  use, so you can join and stay in those too.
- **No more error 319 kicks.** Servers no longer drop you shortly after
  joining.
- **Voice chat that keeps working.** Your microphone still works after a
  Roblox update.
- **Logging in works.** Verification checks and captchas finish instead of
  hanging.
- **The current Roblox look.** The new home screen and side menu, like on
  Windows.
- **Correct device info.** Roblox sees your real RAM and screen size.
- **Smoother mouse and a cooler PC.** Optional raw mouse input, and less CPU
  use while idle.
- **Overlays.** Your frame rate, your ping, and the server's location, drawn
  over the game.
- **Discord.** Shows the game you are playing.
- **A settings window.** Every option in one place.
- **Better desktop launcher.** Runs on Wayland and always finds the installed
  binary.

Everything else works the same as upstream. See
[`textures/`](textures/README.md) for custom textures, a ready-made crosshair
and the asset overlay folder, the [roadmap](docs/ROADMAP.md) for what is
coming next, the [changelog](CHANGELOG.md) for what each release changed,
[performance](docs/PERFORMANCE.md) for CPU load, heat and the knobs that change
them, and the [FAQ](FAQ.md) for Discord RPC, logs, and VR.

## What this fork adds

This repository started as a clone of
[CoderDayton/nightcap](https://github.com/CoderDayton/nightcap) and now runs
ahead of it. Work from that repository is merged in as it lands, so the gap is
the list below and nothing else. In a clone with both remotes,
`git diff coderdayton/main main` is the same list.

- **Joining a private server from a link.** Roblox's `/share?code=` link and
  the join dialog turn into a host launch, and the browser closes itself. A
  public server link works as well, and one Roblox refuses says so instead of
  failing quietly.
- **Rust texture kernels.** The resampler, the EAC alpha merge and the R11 and
  RG11 decoders live in `rust/` and link into the `libvulkan.so` stub.
  `kernel_parity_test` runs the Rust and C++ kernels over every format, stride
  and size and compares them byte for byte; the measured ratios are in
  [PERFORMANCE.md](docs/PERFORMANCE.md). `MOCKTAIL_RUST_KERNELS=off` selects the
  C++ ones.
- **A startup timeline.** `make startup-timeline` prints a per-phase median over
  several launches, so you can see which phase a launch spends its time in.
- **The fullscreen flash and the one-second resize are gone**, along with the
  swapchain-out-of-date report that used to fire when the surface moved under a
  present.
- **Stall reports name what stalled.** A long frame gap reports the thread and
  the wait underneath it rather than a bare duration.
- **Controller glyphs follow the pad.** The family comes from the hardware type
  SDL reports, so a Switch Pro or a GameCube pad draws the nearest layout the
  client has instead of generic buttons.
- **Per-payload graphics fixups.** A compatibility profile can declare
  `swapchain_fixup`, `shader_fixups` and `use_new_text_renderer` for a Roblox
  build that needs them. Every one is absent by default.
- **`window.close_on_leave`** closes Nightcap when you leave an experience
  rather than leaving it open on the Roblox home screen.

## Install with Flatpak

```bash
flatpak install --user https://coderdayton.github.io/nightcap/nightcap.flatpakref
flatpak run io.github.CoderDayton.nightcap
```

Updates arrive through `flatpak update`. Nightcap is not on Flathub yet.

## AppImage

Grab the AppImage from the
[latest release](https://github.com/CoderDayton/nightcap/releases/latest),
make it executable, and run it:

```bash
chmod +x Nightcap-x86_64.AppImage
./Nightcap-x86_64.AppImage
```

## Distribution packages

Each release also carries a DEB, an RPM, and a pacman package, with a
`.sha256` beside every file on the
[releases page](https://github.com/CoderDayton/nightcap/releases/latest):

```bash
sudo apt install ./nightcap_<version>_amd64.deb
sudo dnf install ./nightcap-<version>-1.x86_64.rpm
sudo pacman -U nightcap-<version>-1-x86_64.pkg.tar.zst
```

They install `/usr/bin/mocktail`, so they conflict with upstream's `mocktail`
package.

On Arch Linux, [`nightcap-git`](https://aur.archlinux.org/packages/nightcap-git)
is on the AUR. It builds the current `main` branch and is maintained by
[@dim-ghub](https://github.com/dim-ghub):

```bash
yay -S nightcap-git
```

Nightcap is in no other distribution repository.

### Nightly builds

A prerelease called [`nightly`](https://github.com/randomstuffhere1/nightcap/releases/tag/nightly)
carries every artifact built from the same commit. Every push to `main` publishes
a new one:

| Artifact | x86_64 | aarch64 |
| --- | --- | --- |
| `Nightcap-x86_64.AppImage` (with a `.zsync`) | yes | no |
| `Nightcap-x86_64-portable.tar.zst` | yes | no |
| `.pkg.tar.zst` | yes | no |
| `.deb` | yes | yes |
| `.rpm` | yes | yes |

Each asset has a `.sha256` beside it, and `SHA256SUMS` lists the lot.

```bash
curl -LO https://github.com/randomstuffhere1/nightcap/releases/download/nightly/Nightcap-x86_64.AppImage
chmod +x Nightcap-x86_64.AppImage
./Nightcap-x86_64.AppImage
```

The tarball is a self-contained bundle with its own JRE and WebKit, so it runs
on a distro that has neither. `appimageupdate Nightcap-x86_64.AppImage` picks
up later builds.

aarch64 has no AppImage, tarball or pacman package yet. The portable packager
still assumes x86-64, and Docker Hub dropped arm64 from the Arch base image the
pacman package is built in. The `.deb` and `.rpm` cover aarch64 for now.

Treat these as builds from an untested commit. Use a tagged release for normal
play.

Or build and install from source using the steps under
[Building](#building).

## How it works

```
Roblox APK -> signature and ABI checks -> Bionic + JNI -> SDL3 + Vulkan/OpenGL
```

The APK is checked before any native code is loaded. It is downloaded on first
launch and is not bundled with Nightcap. The last working copy is kept in case
an update fails.

A leftover `.previous_good-recovery.json` in the payload store marks pending
recovery after a publication or restoration failure and blocks further payload
publication and garbage collection. Check `current.json` and `previous_good.json`
against the saved recovery state and fix the underlying problem before manually
removing the marker.

<details>
<summary>Screenshots</summary>

![Roblox home in Nightcap](assets/screenshots/flatpak-home.png)

![Roblox gameplay in Nightcap](assets/screenshots/flatpak-gameplay-tower.png)

</details>

## FFlag overrides

Put a JSON object in `$XDG_CONFIG_HOME/mocktail/fflags.json` (usually
`~/.config/mocktail/fflags.json`). Nightcap applies it on the next launch.
The Advanced page of the [settings window](#settings) edits this file and
does not save text that Nightcap could not load.

PC profiles use the legacy Charts page by default to avoid the blank screen
caused by `Color3` errors in the newer SDUI page. An explicit
`FFlagLuaAppChartsAppPage` override takes precedence over this default.

PC profiles also set `FIntFlyoutTestVariantRollout12` to `2`, which shows the
desktop side panel menu. An explicit override takes precedence.

PC profiles compile the app's menus to native code with
`FFlagDebugLuauCodegenAll`, and turn it off while you are in a game. Setting
it yourself turns the switching off and your value applies everywhere. See
[docs/PERFORMANCE.md](docs/PERFORMANCE.md#native-code-in-the-menus).

```json
{
  "FFlagExample": "True",
  "DFIntExample": "120"
}
```

## Settings

Open the settings window from the app menu: right-click Nightcap and pick
Settings. Or run:

```sh
mocktail --settings
```

With the Flatpak, run
`flatpak run io.github.CoderDayton.nightcap --settings`. To open it with a
key, bind that command to a keyboard shortcut in your desktop's settings. If
Settings is missing from the menu right after you install, log out and back
in.

The window edits `~/.config/mocktail/config.yaml`. It keeps your comments
and layout, saves each change at once, and does not save a value Nightcap
could not start with. The overlay settings apply at once, even in game.
Other changes apply the next time Nightcap starts. If Nightcap is open, the
window offers to restart it, except in the Flatpak.

You can also edit the file by hand. Useful options:

- `graphics.frame_rate_limit: display` to unlock FPS to your monitor's rate.
- `graphics.vsync: off` for the lowest lag, at the cost of tearing.
- `graphics.present_mode: fifo`, `mailbox`, or `immediate` to pick the
  presentation mode yourself instead of letting `graphics.vsync` decide.
- `graphics.backend: opengl` if Vulkan does not work on your GPU.
- `graphics.quality: 1` to `6` to override Roblox's graphics level, or `auto`
  to leave it to the client.
- `performance.texture_memory_mb` to raise or lower the texture budget Roblox
  is told about. 0 keeps the automatic figure.
- `performance.small_texture_upscale` to change how much small compressed
  textures are enlarged, `1` to `8`. `1` turns it off.
- `performance.multithreaded_rendering: true` to size Roblox's worker queues
  from every physical core.
- `graphics.pipeline_cache: clear` to drop the fallback pipeline cache and run
  the next session uncached.
- `audio.buffer_samples` to request a playback buffer size. SDL3 cannot set it
  per device, so the value is advisory and startup reports the size in use.
- `compatibility.desktop_playability: false` to stop telling the client it is on
  a desktop platform.
- `window.close_on_leave: true` to close Nightcap when you leave an experience
  instead of staying on the Roblox home screen.
- `window.exclusive_fullscreen: true` to use a native display mode in
  fullscreen instead of the compositor's borderless path. It can cut input lag
  on X11, but switching takes longer, and Wayland stays borderless.
- `input.pump_hz` to set how often Nightcap checks for mouse and keyboard
  events, from 60 to 360. The default of 240 matches Roblox's highest frame
  rate; lower it to save CPU, or raise it for a display above 240 Hz.
- `overlay.location: true` to show which region the game server is in after you
  join, `overlay.fps` and `overlay.ping` for the performance overlay, and
  `overlay.position` to move it to a corner.

Each of these also has a row in the settings window, which is easier than
editing YAML by hand.

`config/mocktail.example.yaml` documents every option, and
[PERFORMANCE.md](docs/PERFORMANCE.md) explains what the ones that trade CPU for
latency actually do.

## Building

Linux `x86_64` is supported. Experimental FreeBSD 15.1 Linuxulator support has
been tested with an `x86_64` Fedora 44 userspace. On FreeBSD, Nightcap runs
inside Linuxulator; it is not a native FreeBSD binary. Building requires CMake
3.20+, Git, pkg-config, LLD, binutils, a C++17 compiler, SDL 3.4+, SDL3_ttf,
Vulkan, EGL, libplacebo, fontconfig, libcurl, OpenSSL, libelf, libyaml, libpng,
minizip, Capstone 5, utf8proc, nlohmann/json, GTK4, libadwaita 1.6+, and
WebKitGTK 6.0. FFmpeg's libavcodec is optional: without it, in-game video
stays black.

The texture kernels live in Rust (`rust/`, built by `cmake/MocktailRust.cmake`
into a staticlib that links into the `libvulkan.so` stub and the kernel tests).
A Rust toolchain is also required, 1.83 or newer; `rust/rust-toolchain.toml`
selects the channel for rustup-managed installs, and the distribution packages
below carry one. The crates are vendored under `rust/vendor`, so no build path
needs network access. Cross-compiling to `aarch64` needs that target installed
(`rustup target add aarch64-unknown-linux-gnu`) plus
`aarch64-linux-gnu-gcc` on the linker path, and
`CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc` set for
the build, since a native aarch64 build uses the host compiler instead.
`-DMOCKTAIL_BUILD_RUST_KERNELS=OFF` builds against a prebuilt archive given in
`-DMOCKTAIL_RUST_PREBUILT=/path/to/libmocktail_rs.a`.

Cargo's build output is redirected into `build/rust-target`, so it is gitignored
and `make clean` removes it along with the rest of the build tree.

See the [FreeBSD Guide](packaging/freebsd/README.md) for setup and launch instructions.

<details>
<summary>Ubuntu 26.04+</summary>

```bash
sudo apt update
sudo apt install build-essential cmake git ninja-build pkg-config lld \
  libsdl3-dev libsdl3-ttf-dev libcurl4-openssl-dev libssl-dev \
  nlohmann-json3-dev libyaml-dev libpng-dev libelf-dev libminizip-dev \
  libcapstone-dev libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \
  libutf8proc-dev libfontconfig1-dev libegl-dev libvulkan-dev \
  libplacebo-dev libavcodec-dev zlib1g-dev rustc cargo
```
</details>

<details>
<summary>Arch Linux</summary>

```bash
sudo pacman -S --needed base-devel cmake git ninja pkgconf lld sdl3 sdl3_ttf \
  curl openssl nlohmann-json libyaml libpng libelf minizip capstone gtk4 \
  libadwaita webkitgtk-6.0 libutf8proc fontconfig libglvnd \
  libplacebo ffmpeg vulkan-headers vulkan-icd-loader zlib rust
```
</details>

<details>
<summary>Fedora 44+</summary>

```bash
sudo dnf install gcc-c++ cmake git ninja-build pkgconf-pkg-config lld \
  SDL3-devel SDL3_ttf-devel libcurl-devel openssl-devel \
  nlohmann-json-devel libyaml-devel libpng-devel elfutils-libelf-devel \
  minizip-ng-compat-devel \
  capstone-devel gtk4-devel libadwaita-devel webkitgtk6.0-devel \
  utf8proc-devel fontconfig-devel libglvnd-devel vulkan-headers \
  vulkan-loader-devel libplacebo-devel ffmpeg-free-devel zlib-ng-compat-devel rust cargo
```
</details>

```bash
git clone --recurse-submodules https://github.com/CoderDayton/nightcap.git
cd nightcap
make build
./build/mocktail
```

To install it for your user, with an app menu entry and Roblox link handling:

```bash
make install PREFIX=~/.local
```

Use `sudo make install` instead to install system-wide under `/usr`. To make
Roblox website links open the build tree copy without installing, run
`make register-url-handler`.

To build and install the Flatpak from source, install `flatpak-builder` and run
`make flatpak`. It builds with 2 parallel jobs to keep memory use low. Pass
`FLATPAK_JOBS=N` to `make flatpak`, or set `MOCKTAIL_FLATPAK_JOBS=N` for
`scripts/build_flatpak.sh`, to change that. To build without installing, run
`make flatpak-build`; the result stays in `build-flatpak/`.

To see where startup time actually goes, run `make startup-timeline`. It prints
a per-phase median across a few launches; see [Startup](docs/PERFORMANCE.md#startup)
for what the phases are and which ones the caches shorten.

To run the same checks as CI before each commit and push, install
[lefthook](https://github.com/evilmartians/lefthook) and run once:

```bash
lefthook install
```

## Known limitations

Nightcap does not currently work with `hardened_malloc`. Using
`hardened_malloc` may cause Nightcap to fail to start or crash during runtime.
Run Nightcap without `hardened_malloc` enabled.

## License

[Apache License 2.0](LICENSE). Third-party components keep their own licenses.

## Support

Support this fork by giving it a star or with cryptocurrency:

- LTC: `LdABR2ELRYrUESWZWrEk1uNx38gHHMuHUU`
- SOL: `tjeDnPoWeyW8zfTf8c5CVAm5QTV8vFPaPouaScAdwfT`

Support the upstream project, which does most of the heavy lifting:

- USDT (TON): `UQCi6Yzcc9cOctoij6n_r1K90-OdVxAT0D_xo2UzGKkQaJDY`
- USDT (TRC20): `TNPMG9Vig2xiuo2r1QqnXRChPH7Vu28Jmx`
