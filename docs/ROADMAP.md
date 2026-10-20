# Nightcap roadmap

What Nightcap is working toward, in priority order. Items move to
[Shipped](#shipped) when they land on `main`. Open an issue to propose a
change to this list.

## 1. Settings UI

Shipped.

`mocktail --settings`, or Settings from the app menu, opens a GTK4 window
covering every `config.yaml` setting: graphics, input, performance, audio,
network, Discord, Fleasion, updates and the device identity, with a text editor
for `fflags.json` and a button that opens the config folder. Each row accepts
only values the loader accepts, so a bad value is refused where it is typed
instead of at the next launch. A setting changed from its default gets a reset
button, and a running Nightcap is offered a restart with its own options and
environment.

**It edits `config.yaml` itself, and keeps the comments.** The window replaces
only the bytes that carry the key it changes: comments, order, line endings and
unrelated keys are left as they are, a commented-out key is uncommented in
place, and every result is parsed again and refused if it would change any other
setting. That removes the surprise the earlier sidecar design had, where a value
set in the window outranked a later hand-edit of the same key.

## 2. Controller button glyphs

Shipped, with a limit worth knowing. The family is now taken from
`SDL_GetRealGamepadType`, the hardware's own type, which is not affected by a
remapping. SDL's reported type returned `SDL_GAMEPAD_TYPE_STANDARD` for
anything it could not place, so a Switch Pro, a Joy-Con or a GameCube pad
arrived as an unknown pad and drew generic glyphs.

The client has glyph sets for PlayStation and Xbox pads and nothing else, so a
Switch or GameCube pad is reported as the nearest layout the client does know,
which is Xbox: both put their face buttons in the same diamond with A and B
where Xbox puts them. Reporting them as PlayStation would put the art on the
wrong physical buttons, since PlayStation has a diamond in a different place
with A and B swapped. Startup prints `closest match: the client has no glyph
set for this pad` when it does this, so the approximation is visible rather
than looking like a bug.

No invented type value is ever sent. Only 0 to 3 are known to be understood by
the client, and a value outside that set could leave it with no art to draw at
all, which is worse than the wrong art. The mapping is a tested pure function
of the SDL type, so a future SDL type that needs its own family fails a test
rather than silently drawing generic glyphs.

Glyph rendering can only be confirmed with a pad plugged in; there is no
automated coverage of the client actually drawing them.

## 3. Load and join freezes

**Investigated. No host-side cause found; probably not actionable from here.**

The freezes are real (a 78 s session lost 15.5 s across 33 frames, and a 35 s
one lost 13.4 s across 32), but every layer instrumented says the engine is
*idling*, not blocked. Full numbers in
[PERFORMANCE.md](PERFORMANCE.md#when-the-engine-stops-rendering); the short
version:

- The main thread pumps at a constant 4.17 ms through a freeze, the same as
  when frames are flowing, at 7.5% busy against 9.0%.
- The render thread drops to 0.2% busy, from 3.2%. It has nothing to do.
- Under `strace`, 141 s containing multi-second freezes produced three network
  reads over 50 ms, all at startup. Nothing waits for anything close to a
  freeze duration. There were 21,616 `EAGAIN` returns from non-blocking reads:
  polling, not waiting.
- Gap durations cluster near exact second multiples, mean 998.7 ms, which reads
  like a retry or poll interval.

The engine is calling into itself on schedule, finding no work, and not
rendering. That is most likely engine or server-side behavior reached through
the guest.

Two things were chased and disproved, recorded so they are not chased again. A
shader-compilation cause was ruled out first: all pipeline and texture work
totalled 40 ms in a 78 s session. Then a DNS storm looked conclusive:
`inet_addr` called 12,408 times for one address, 808 times in a second during
the largest gap. But per-gap coverage showed fourteen of the ~1 s gaps contain
essentially none of it, and the session-wide correlation is r = −0.07. It
explains one gap and nothing else.

- Not a Nightcap-side fix, on the evidence so far. If someone wants to pursue it
  further, the remaining lead is engine-internal, and `perf record -g` is the
  only tool left that has not been tried: it samples every thread, so unlike
  `--profile` it can see a spin that emits no Vulkan or pump slices, and unlike
  `strace` it does not perturb per-syscall cost.
- Do not read a low `cpu_us` on a pump slice as a blocked thread without also
  checking the cadence. An idle polling thread and a blocked one look the same
  on CPU time alone, and that misreading is what sent this investigation down
  two wrong paths.

## Why item 3 was rewritten

This item was written as "first-load stutter" and assumed shader compilation
was the cause. A recorded trace says otherwise. Over a 78 s session in an
experience, all pipeline and texture work in the adapter together came to
40 ms, which is 0.05% of wall time:

| slice | count | total |
| --- | --- | --- |
| `vkCreateShaderModule` | 2271 | 21.79 ms |
| `vkCreatePipelineCache` | 2 | 14.66 ms |
| `vkCreateGraphicsPipelines` | 130 | 3.54 ms |

All 132 pipeline creates already passed a cache handle, and the two
`vkCreatePipelineCache` calls report `initial_bytes` of about 7.2 MB: the
client loads its own `shadercachevk.bin` and passes it to the driver. The
shader cache was working. Compiling shaders is not the stutter.

The stutter is elsewhere. Frame interval p50 was 16.7 ms and p99 89.6 ms, and
33 frames of 3490 ran over 100 ms, 0.9% of frames but 15.5 s of lost time
out of 77.5 s. The worst of them cluster near exact multiples of a second:

```
1824.6ms   1415.1ms   1354.0ms   1001.4ms   1000.0ms
 999.9ms    998.6ms    994.0ms    945.5ms
```

Inside the largest 1825 ms gap the adapter did almost nothing: presents stop
and resume, and the only large slice is the engine's own main-thread step at
347 ms. Comparing that step's `cpu_us` against its duration showed it was
blocked, not busy, which is what the item above now says.

## Shipped

- Settings window, from the app menu or `mocktail --settings`: graphics,
  input, performance, audio, and Discord options, and a text editor for
  `fflags.json`. It edits the same `config.yaml`, keeps your comments, and
  does not save a value Nightcap could not start with. Overlay changes apply
  at once; the rest apply on the next launch. A setting changed from its
  default gets a reset button, and a running Nightcap is offered a restart with
  its own options and environment.
- A pipeline cache fallback. The client already persists its own
  `shadercachevk.bin` and hands it to `vkCreatePipelineCache`, so this does
  nothing on a stock client; it only engages if a future client stops seeding
  the cache itself. `graphics.pipeline_cache` turns it off, or clears it for
  one session. See [PERFORMANCE.md](PERFORMANCE.md).
- Lower input latency: frames are presented as soon as they are ready.
- Controllers hot-plug without a restart, and a pad that is unplugged mid-game
  releases its held buttons and sticks instead of leaving them stuck.
- Mouse look keeps responding after a long turn, instead of going dead until
  the pointer travels back from past the screen edge.
- Optional raw mouse input: `input.raw_mouse` in `config.yaml` sends look
  deltas in host units, so fractional display scaling no longer changes
  first-person sensitivity.
- Small textures upscaled 4x by default, with PNG overrides in
  `~/.config/mocktail/textures`.
- ETC2 textures decoded on desktop GPUs without stutter.
- The texture hot kernels in Rust: resampling, the EAC alpha merge and the R11
  and RG11 decoders, checked against the C++ they replaced by
  `kernel_parity_test`. The ratios are in [PERFORMANCE.md](PERFORMANCE.md).
- No more error 319 kicks after joining a server.
- Roblox sees the real host RAM and screen size.
- Desktop launcher that works on Wayland and finds the installed binary.
- Discord Rich Presence on by default, with the experience name, creator,
  elapsed time, the experience icon with a Roblox badge, and a join button
  when the server is public. See the [FAQ](../FAQ.md) to switch it off.
- Custom Discord presence: title, details, state, and large and small icons
  from `config.yaml`, with `{place_name}`, `{place_icon}`, and
  `{creator_name}` placeholders.
- `--profile` writes a Chrome-format trace of the Vulkan adapter's work,
  including shader and pipeline creation, readable in `ui.perfetto.dev`.
  [BENCHMARKING.md](BENCHMARKING.md) fixes the experiences to measure and
  `scripts/summarize_profile_trace.py` builds the before and after table.
- Texture uploads no longer stall the frame they land on: ETC2 decode and
  resampling run on a persistent worker pool.
- Less stutter while joining an experience and while textures load in. See
  the [changelog](../CHANGELOG.md) for the numbers.
- The host main thread rests between engine polls instead of spinning, and
  GameMode's core pinning is released, so Roblox's workers spread across
  every core. See [PERFORMANCE.md](PERFORMANCE.md).
- Own app ID `io.github.CoderDayton.nightcap`, so Nightcap installs next to
  upstream.
- AppImage and signed Flatpak releases on every `v*` tag, with the Flatpak
  repo on GitHub Pages.

## Not planned

- Replacing the `mocktail` binary and config names. Keeping them makes
  upstream merges clean.
