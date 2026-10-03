# Magnesium Client

C++17 raylib/raygui client for Sodium API. Sodium React fonts and their license
remain bundled. Stable IRC nickname colors, alternating message backgrounds,
Unicode wrapping, history scrolling, resizable HighDPI rendering, and composer
focus after sending are retained. Controls use slightly wider spacing, softer
colors, a masked password field, and readable connection/keyring status.

## Build and run

Install CMake, a C++ compiler, libcurl development files, and Python 3 (for tests).
CMake fetches raylib; use the existing build directory to reuse its checkout.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --build build --config Release --target package
```

On macOS the app is `build/magnesium.app`; launch with `open build/magnesium.app`
or run `build/magnesium.app/Contents/MacOS/magnesium`. Packaging produces an unsigned
macOS ZIP containing the complete app with its fonts. The app uses macOS system
frameworks/libcurl and a statically linked raylib.

On Linux install `libsecret-1-dev`, `libcurl4-openssl-dev`, pkg-config, and the X11,
OpenGL, and audio development dependencies required by raylib. The native keyring
backend requires a running Secret Service, such as GNOME Keyring or KWallet's
Secret Service implementation. The ZIP is an install layout with `bin/magnesium`
and `bin/assets`; runtime libsecret, libcurl, and platform dependencies must also
be installed on the destination OS.

On Windows use a Visual Studio compiler and a libcurl CMake package (for example
vcpkg with the CMake toolchain file). CMake selects Credential Manager/Advapi32
and raylib's Windows dependencies, without Linux GL/X11 libraries. Keep any
libcurl runtime DLLs and dependencies with the executable, or use a static libcurl
build. Fonts stay in `assets` beside the executable. CPack builds native ZIPs on
the host OS; it does not cross-compile or automatically bundle third-party DLLs.

## Remembered sessions

After registration or login, the entire server address, username, and token record
is saved as one native secret: macOS Keychain (`SecItem`), Windows Credential
Manager, or Linux Secret Service via libsecret. No password is saved and there is
no token file fallback. The credential service/target is
`us.tamastudios.magnesium.session`; only the most recent account is remembered.

At startup a background worker loads the saved record and validates it against
`GET /sodium/v1/users/info` before entering chat. Invalid/expired bearer tokens
are removed and return to sign-in. Server failures and malformed responses keep
the saved record, remain on sign-in, and offer **Retry saved session**. If the
keyring is unavailable or access is denied, sign-in still works for the current
run and chat shows that it could not remember the session. Linux keyring calls
have a 15-second cancellation deadline. OS permission prompts may require a user
response; keyring work runs outside the drawing thread.

Logout clears the in-memory token and deletes the native credential. If the OS
refuses deletion, sign-in explicitly reports the failure and offers **Retry keyring
removal**. Complete removal then (or remove the credential with the OS keyring
manager); an inaccessible keyring cannot be guaranteed cleared. Keyring save and
logout operations are serialized to prevent a late save from undoing logout.

Chat detects HTTP 401/403 during periodic sync, clears the session and removes the
saved credential. Network failures retain the active session and automatically
retry. No token or password is logged. Server URLs accept HTTP(S) without embedded
credentials, queries, fragments, or control characters. Use HTTPS for remote
servers; local loopback HTTP matches Sodium's development default.

Sodium accounts migrated from a hash-only database can continue using a remembered
legacy token. If password login says a reset is required, explicitly reset the token
through the server API, then sign in. Login itself does not invalidate legacy tokens.

## Verification (October 3, 2026)

The macOS ARM64 client was rebuilt. Four CTest tests passed: chat wrapping/scroll
layout, bounded history, saved-record and URL validation, and background session flows using a
loopback HTTP fixture with a mocked keyring. The flow tests cover missing/unavailable
keyrings, corrupt records, successful restore, invalid-token deletion, failed
deletion, malformed/wrong-user responses, server outages, registration/login, and
legacy reset-required responses.

`build/keyring_probe --isolated-roundtrip` passed native macOS Keychain
save/load/delete using the separate `us.tamastudios.magnesium.session.test` service.
It does not overwrite the client's actual saved session. This probe is manual,
not a normal CTest test, because it may require OS keyring access. Windows and Linux
backends have been implemented but have not been compiled or runtime-tested on
those operating systems. No Windows/Linux client binaries were produced.

## Memory use

Startup now fetches the newest 50 messages rather than replaying the entire server
history. The client retains at most 512 messages in a movable window under a 1 MiB **accounting
budget** for message text, objects, and estimated row metadata. This is a history
budget, not a limit on total process RAM. Earlier messages remain on Sodium and can be loaded on demand:

- Scroll upward at the top of the log, or click **Older**, to fetch the preceding
  up to 50 messages. Pages of unusually long messages are applied in smaller
  contiguous portions so none are skipped. The currently visible message stays in place when retained.
- **Newer** (or scrolling downward at the bottom while browsing history) fetches
  the next page. **Latest** jumps straight to the current server tail.
- Browsing history pauses live polling so incoming messages cannot displace your
  reading window. Polling resumes when Newer reaches the tail or you click Latest.
- Pages replace rows at the opposite end as the message/byte/renderer budget fills;
  you can fetch those rows again in either direction. Network errors retain the
  displayed history and allow a retry. Invalid tokens return to sign-in.

Use an updated Sodium server with the `before` pagination cursor. No plaintext
history cache is written to disk. Logout/session expiry
clears the owned message/layout allocations.

Wrapped text is cached until history, viewport width, or font DPI changes, with
at most 8,192 cached rows. Newline-heavy messages therefore cannot create an
unlimited renderer cache. Rows omitted by this budget are retired from the
message window too, keeping pagination cursors aligned with visible messages. Username colors and alternating backgrounds stay stable
when old messages are retired; rendering still clips/scissors and scrolls normally.

HTTP response bodies are limited to 1 MiB, sync pages to 50 messages, and only one
send can be in flight. Trying Enter again while sending keeps the next draft in the
composer instead of spawning more threads. The Send button shows **Sending...**.
Sodium React fonts are rasterized for the display DPI (32 px regular / 64 px heading
on a 2× Retina screen), with reload on a DPI change. CPU glyph pixel copies are
released after the GPU atlas is created; glyph metrics, the GPU texture, and Retina
rendering are retained. This renderer does not use CPU `ImageDrawText`.

Local macOS ARM64 measurements (October 3, 2026):

| Scenario | Before memory fixes | After |
| --- | --- | --- |
| Idle real client, RSS | 96.8 MiB | 92.1 MiB |
| Idle real client, macOS physical footprint | 129.2 MiB | 124.8 MiB |
| Synthetic 100,000 short messages, physical footprint | 148.6 MiB | 125.8 MiB |
| Same synthetic backlog, retained messages | 100,000 | 512 |
| Same backlog, two render frames including first layout | 4,257 ms | 65 ms |

The synthetic probe uses a mocked keyring and no network, with identical window,
font assets, messages, and rendering backend. The real idle samples include native
Keychain startup. Process footprint includes graphics/system overhead and varies
with macOS memory pressure; RSS is a different metric. A reported 2× idle regression
was not reproduced locally: pre-keyring and pre-fix physical footprints were about
128 MiB and 127 MiB in the initial comparison. The memory fixes address measured
backlog growth and allocation churn, without claiming a 50% idle reduction or
cross-platform profiling that did not run.

For a manual drawing/memory regression probe (opens a temporary window):

```sh
./build/client_memory_probe 100000 2
```

The probe also checks cache reuse, resize invalidation, newline-heavy row budgets,
HTTP body bounds, draft preservation while a send is in flight, and cleared history.
It is a manual target because it needs a graphics display; ordinary CTest remains
headless. Windows and Linux builds/memory measurements have not been run.


### Idle renderer memory update

Raylib is pinned to the exact revision already used locally, rather than floating
`master`. The GLFW Desktop build requests no depth/stencil framebuffer, because
the client uses ordered 2D drawing and never enables depth/stencil testing. A
narrow reset-hint hook in `src/graphics_init.c` applies this after raylib resets
GLFW defaults. Color buffers, double buffering, Retina resolution, fonts, and the
60 FPS target are unchanged. If adding 3D or stencil effects later, remove this
hook or restore the required framebuffer attachments.

The default render batch is 1,024 quads instead of 8,192; raylib automatically
flushes full batches, so this is not a glyph or message limit. Unused raylib audio
and 3D model modules are excluded. No existing client sound/3D feature is removed.

Paired local macOS ARM64 login-page runs measured physical footprints of
127.0/128.8 MiB before and 118.4/118.7 MiB after: about 9.4 MiB (7.3%) lower on
average. RSS varied and did not decrease in these samples; the result is a
physical-footprint reduction, not a claim that every memory metric improved.
Windows/Linux memory savings have not been measured. Native graphics checks
verify zero depth/stencil bits, correct drawing order and scissor clipping after
10,000 rectangles force batch flushes, larger-window rendering, and the existing
font, pagination, bounded-history and focus regressions.
