# RPiPlay webOS Port — Technical Documentation

AirPlay mirroring receiver running natively on LG webOS TVs. iPhone screen
mirroring works directly on the TV with no external hardware.

**Target:** LG 49UF850V — webOS 2.2.3, MStar LM15U SoC, ARMv7 soft-float,
kernel 3.10.23, glibc 2.19, GStreamer 1.2.4.

**Branch:** `webos-port` (upstream is FD-/RPiPlay — never push there).

---

## Architecture overview

```
iPhone (AirPlay mirroring)
    │
    │  H.264 + AAC-ELD over AirPlay
    ▼
RPiPlay (native ARM binary, runs as root)
    │
    │  RTP/MPEG-TS over UDP to 127.0.0.1:5000
    ▼
com.rpiplay.airplay (webOS web app)
    │
    │  <video src="miracast://miracast">
    │  with mediaOption describing the stream
    ▼
TV's Miracast media pipeline (umediaserver)
    udpsrc → rtpjitterbuffer → rtpmp2tdepay → tsdemux
    → h264parse → omxh264dec → mvsink (video plane)
    → aacparse → omxaacdec → audio output
```

RPiPlay receives the AirPlay stream, remuxes H.264 into MPEG-TS/RTP, and sends
it to a local UDP port. A transparent webOS web app plays that port through the
TV's built-in Miracast pipeline — the same mechanism the stock Screen Share app
(`com.webos.app.miracast`) uses. This means umediaserver and the compositor own
the video plane, so we never have to fight the display stack.

### Why the Miracast trick

The TV's MStar SoC has two composited hardware layers:
1. **GOP (graphics)** — the Wayland compositor draws the UI here. Opaque, always on top.
2. **MVOP (video)** — the hardware video plane, always underneath.

A standalone GStreamer pipeline writing to mvsink would render into the MVOP
plane, invisible under the opaque GOP. Commercial apps make a region transparent
by registering with umediaserver (`com.webos.media/load`), which orchestrates
the compositor. Hooking into that orchestration from outside is fragile and
version-specific.

The Miracast path sidesteps all of this: the web app is a regular foreground
window with `"transparent": true` in appinfo.json, and the TV's own media
pipeline handles everything below it. It works because LG already solved the
display-stack problem for Miracast.

---

## Video path

**Source file:** `video_renderer_webos.c`

### GStreamer pipeline (RPiPlay side)

```
appsrc (H.264 NALUs from AirPlay)
  → h264parse
  → mpegtsmux
  → rtpmp2tpay
  → appsink (send_rtp callback)
```

`send_rtp` paces packets to `SEND_RATE` (6 MB/s) in bursts of at most
`SEND_BURST` (48 KB) to avoid overflowing the TV's 160 KB UDP receive buffer.
Packets go to 127.0.0.1:5000 via a raw UDP socket (no GStreamer udpsink — we
need pacing control for GOP replays).

When `RPIPLAY_NO_AUDIO=1`, the pipeline has no audio appsrc, so mpegtsmux
operates on video only (removing ~27 ms of latency — see Performance below).

### Connection startup

iOS sends an IDR only when mirroring starts. The TV decoder must be running
before the first frame arrives, so `conn_init` (on the RAOP HTTP thread):

1. Launches the web app via `luna://com.webos.applicationManager/launch` with
   UDP port, resolution, and framerate as launch params.
2. Polls the TV for UDP_PORT's socket to appear (`/proc/net/udp`).
3. Waits `TV_SETTLE_MS` (1000 ms) for the pipeline to fully start.

iOS blocks on the HTTP response, so it waits naturally.

### GOP replay

Every H.264 frame since the last IDR is kept in a ring buffer (`GOP_MAX_BUFFERS`
= 600 frames, `GOP_MAX_BYTES` = 8 MB). When the TV pipeline restarts (rotation,
reconnect), the cached GOP is replayed through `send_rtp` so the decoder has a
reference frame immediately.

The first replay to a newly started pipeline often produces nothing: it is what
makes the pipeline add its tsdemux pads and start the hardware decoder (seen in
LG's kernel log as `MApi_VDEC_EX_V2_Init`). The burst sent during init is lost.
Fix: retry after `REPLAY_RETRY_MS` (1 s), then restart the pipeline entirely
after `PICTURE_TIMEOUT_MS` (2 s).

### App events (port 7737)

RPiPlay runs an HTTP listener on `APP_EVENT_PORT` (7737) bound to localhost. The
web app reports `<video>` events and umediaserver status back here using
fire-and-forget image requests (`new Image().src = ...`). RPiPlay logs these as
`[webos-app]` lines and uses them to detect "playing" (picture visible) vs. no
picture (needs another replay).

The same port serves `/time` (RPiPlay's millisecond clock) and `/clock`
(clock.html) for the latency test.

---

## Audio path

**Source file:** `audio_renderer_webos.c`

AirPlay mirroring audio is AAC-ELD (44.1 kHz stereo, 480 samples/frame). The
TV's Miracast pipeline expects AAC-LC in the MPEG-TS stream, so:

1. **Decode** AAC-ELD with fdk-aac (`aacDecoder_Open` with `TT_MP4_RAW`)
2. Buffer decoded PCM in a ring (`RING_FRAMES` = 22050 = 0.5 s)
3. **Re-encode** as AAC-LC/ADTS (1024 samples/frame) with fdk-aac
4. Push ADTS frames to the video renderer via `video_renderer_webos_push_audio`

### Pacing thread

mpegtsmux waits for data on every input before releasing a muxed packet. Without
a steady audio feed, video frames stall. A pacing thread emits one AAC-LC frame
per 1024 samples of wall-clock time. When the phone sends nothing (idle screen,
muted), it fills with silence so the mux never blocks.

Prebuffer: `PREBUFFER_FRAMES` (2048 ≈ 46 ms) of PCM must accumulate after a gap
before playback resumes, preventing choppy audio from incomplete frames.

Drop policy: if the ring fills beyond `MAX_LATENCY_FRAMES` (11025 ≈ 250 ms),
the oldest samples are dropped to cap latency.

---

## Volume control

**Source file:** `audio_renderer_webos.c` (lines 220-350)

AirPlay mirroring has no audio stream until something on the phone actually
plays, so volume is handled via RTSP SET_PARAMETER/GET_PARAMETER, not the audio
renderer's normal data path.

### The curve

iOS sends volume as a float 0.0-1.0 in 16 steps. The TV's volume is 0-100.
A linear mapping puts most of the perceptible range in the first few notches, so
a hand-tuned curve maps the 17 positions (0 + 16 steps):

```c
static const int tv_volume_curve[17] = {
    0, 1, 2, 4, 6, 8, 10, 13, 17, 22, 28, 35, 43, 54, 66, 82, 100
};
```

### SET_PARAMETER (phone slider moved)

iOS sends a *relative* change from its last known position, not an absolute
volume. But the user can also change volume with the TV remote, so the two can
drift. The fix:

1. Read the TV's current volume via `luna://com.webos.audio/getVolume`
2. Find where the TV sits on the curve (`curve_position` — nearest match)
3. Compute the step delta from the old iOS position to the new one
4. Apply that delta to the curve position
5. Set the TV to `tv_volume_curve[new_position]` via `luna://com.webos.audio/setVolume`

At either extreme (0 or 100%), they re-sync. Volume 0 also calls `setMuted`.

### GET_PARAMETER (phone asks "where is the slider?")

Called when a mirroring session connects. Returns the TV's current volume mapped
back through the curve, so the iPhone slider starts at the TV's actual level.

---

## Rotation handling

**Source file:** `video_renderer_webos.c`

When the phone rotates, iOS changes the H.264 picture size (e.g. 1920x1080 →
1080x1920). The TV's Miracast pipeline can't follow a mid-stream size change:
the omxh264dec + mvsink configuration is set at pipeline start.

### The fix: relaunch the app

1. RPiPlay detects the size change in the H.264 SPS
2. Signals the web app, which relaunches itself via `webOSRelaunch`
3. The relaunch handler destroys the old `<video>` and creates a new one
   (always 1920x1080 — the TV pipeline scales)
4. RPiPlay waits for the old pipeline's UDP socket to close and the new one to
   open (polling `/proc/net/udp`)
5. Replays the GOP, with the retry logic described above

### Why the first replay often fails

From LG's logs: a brand-new pipeline adds its tsdemux pads and starts the
hardware decoder only when the first data arrives. That first data burst
triggers the init, but is itself lost. A second replay 1 s later works because
the decoder is now running.

Timing: first replay `REPLAY_SETTLE_MS` (300 ms) after the new socket appears,
retry at `REPLAY_RETRY_MS` (1 s) if no "playing" event, pipeline restart after
`PICTURE_TIMEOUT_MS` (2 s). The `PIPELINE_EXIT_TIMEOUT_MS` (5 s) timeout
prevents deadlock if the old pipeline's socket never closes.

### Serialisation

Rotations during a pending restart are queued. The old pipeline must fully exit
before the new one starts — racing them leaves both broken.

---

## mDNS service discovery

**Source file:** `dnssd_mdns.c`

The TV has no Avahi or Bonjour daemon. RPiPlay needs mDNS to advertise itself as
an AirPlay receiver. This file implements just enough of the Bonjour API that
RPiPlay's existing `dnssd.c` interface compiles unchanged:

- `DNSServiceRegister` / `DNSServiceRefDeallocate`
- `TXTRecordCreate` / `SetValue` / `GetLength` / `GetBytesPtr` / `Deallocate`

It joins the 224.0.0.251:5353 multicast group, builds DNS response packets
(SRV, TXT, A, PTR records), and re-announces every `ANNOUNCE_INTERVAL` (20 s).
It responds to mDNS queries matching the service name. The service name comes
from the TV's network name (see Boot chain below).

---

## Build system

### Toolchain

`./webos-build/build.sh` runs a Docker container (`webos-build/Dockerfile`)
with Ubuntu 22.04's `arm-linux-gnueabi` cross-compiler. Inside:

- **OpenSSL 1.1.1w** cross-compiled for ARM soft-float (static)
- **libplist 2.3.0** cross-compiled (static)
- **fdk-aac** compiled from RPiPlay's bundled sources (cached after first build)
- **GStreamer/glib headers** copied from the host, with a hand-written
  ARM32-correct `glibconfig.h` (4-byte pointers/longs)

### Stub linking

GStreamer/glib are only available as shared libraries on the TV. The build
generates stub `.so` files exporting every GStreamer/glib symbol our objects
reference (collected via `nm -u`). These stubs provide link-time resolution so
the linker creates proper PLT entries; at runtime the TV's real libraries supply
the implementations.

### Custom CRT (superseded)

The old toolchain (Debian cross-gcc linking against a copied `tv-sysroot` of
the TV's glibc 2.19) needed a custom `crt1.S` (the compiler's own `crt1.o`
called the glibc 2.34 `__libc_start_main` ABI) and a hand-built `ld`
command line. Since 2026-10-30 the build uses the webosbrew buildroot-nc4
SDK (glibc 2.12 sysroot), whose own CRT matches the target, so plain driver
linking works and `crt1.S` was removed. History: the file called
`__libc_start_main` with `init/fini = NULL` (constructors run via
`.init_array`) after 8-byte stack alignment per AAPCS.

### glibc compatibility shims

**Source file:** `glibc_compat.c`

Written for the old toolchain (Debian glibc 2.34 objects linked against the
TV's 2.19); with the buildroot SDK the dependencies are compiled against
2.12 headers and most rows are unused - the file is kept as cheap insurance
and still provides:

| Symbol | Why needed | Shim |
|--------|-----------|------|
| `fstat/stat/lstat` | glibc 2.33 added direct exports; 2.19 only has `__fxstat`/`__xstat`/`__lxstat` | Calls the `_STAT_VER_ARM=3` versions |
| `atexit` | Lives in `libc_nonshared.a`, not exported from libc.so | Wraps `__cxa_atexit` (GLIBC_2.4) |
| `pthread_atfork` | In `libpthread_nonshared.a`; OpenSSL registers fork handlers | No-op (the TV never forks rpiplay) |
| `__libc_single_threaded` | glibc 2.32; libstdc++ references it | `= 0` (multi-threaded) |
| `getentropy` | glibc 2.25; OpenSSL uses it | `SYS_getrandom` or `/dev/urandom` fallback (kernel 3.10 lacks the syscall) |
| `explicit_bzero` | glibc 2.25 | Volatile byte-by-byte zero |

### Build flags

```
-O2 -march=armv7-a -mfloat-abi=soft -fno-stack-protector
```

No `-D_FILE_OFFSET_BITS=64` or `-D_LARGEFILE64_SOURCE` — those trigger newer
glibc symbol redirections incompatible with glibc 2.19.

### Link order

Since the buildroot SDK switch, linking goes through the compiler driver
(`$CXX`, `-static-libstdc++ -static-libgcc`). Effective order:

1. SDK CRT (crt1.o/crti.o/crtbegin from the glibc 2.12 sysroot)
2. All application .o files (including glibc_compat.o)
3. Static OpenSSL + libplist
4. GStreamer/glib stub .so files (`-Wl,--no-as-needed` keeps the DT_NEEDED
   entries the TV's real libraries satisfy at runtime)
5. Static libstdc++/libgcc (no libstdc++/libgcc_s at runtime)
6. SDK libc/libm/libpthread/librt/libdl — verified requirement: GLIBC_2.4
   (+ one GLIBC_2.7 symbol), so the binary runs on every webOS version

(The pre-2026 build called `arm-linux-gnueabi-ld` directly with a custom
crt1.o and the TV glibc 2.19 from tv-sysroot; see "Custom CRT" above.)

---

## Boot and deployment

### Deploy: `tv-run.sh`

Uploads the binary, startup script, and app files to the TV over SSH (port 22,
root, key `~/.ssh/uf850v_webos`). Chroots into the real rootfs
(`/proc/1/root/bin/busybox chroot /proc/1/root /bin/sh`) because luna-send only
works outside the dev-mode jail. Stops any running instance, then starts the new
one.

Environment variables passed through:
- `RPIPLAY_NO_AUDIO=1` — video-only mode (no audio appsrc, no mpegtsmux audio input)
- `RPIPLAY_LATENCY_TEST=1` — serves clock.html, overlays clock on the TV
- `RPIPLAY_DUMP_DIR=/tmp/rpdump` — saves H.264 sent after each rotation
- `RPIPLAY_ARGS="..."` — extra RPiPlay flags

### Boot: `boot-root.sh` → `rpiplay-start.sh`

`boot-root.sh` lives in the TV's persist directory and runs at boot (after sshd
starts). It launches `rpiplay-start.sh`, which:

1. Kills any leftover supervisors and rpiplay instances
2. Reads the TV's network name from `com.webos.settingsservice/getSystemSettings`
   (retries for up to 2 minutes — the settings service may not be up yet)
3. Starts rpiplay in a respawn loop (`rpiplay -n "$name"`)
4. Logs to `/tmp/rpiplay.log`

RPiPlay is available ~3-4 minutes after power-on and auto-restarts on crash.

### SSH access

The TV runs sshd on port 22 (started by `boot-root.sh`). Connect with:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i ~/.ssh/uf850v_webos -p 22 root@192.168.50.99
```

The `ssh-rsa` flags are needed because the TV's sshd is old (dropbear) and
modern OpenSSH disables RSA by default.

---

## Performance measurements (2026-10-28)

### CPU

| Process | % of one core | Notes |
|---------|--------------|-------|
| RPiPlay total | ~20% | No thread above 4% |
| TV media pipeline | ~29% | omxh264dec + mvsink |

Not a concern — the TV has four cores.

### Latency

Measured with the built-in latency test (see below). Phone mirrors a clock page;
the TV overlays the same clock; display capture shows both readings.

| Mode | Delay | Range |
|------|-------|-------|
| With audio | ~50 ms | 41-59 ms (6 captures) |
| Video only | ~24 ms | 16-30 ms |
| Audio cost | ~27 ms | |

The audio delay comes from two sources:
1. **mpegtsmux** waits for an audio frame (~23 ms between AAC-LC frames) before
   releasing a video frame
2. **TV A/V sync** holds video to match the audio path's latency

### Frame holding in tsdemux

GStreamer 1.2.4's tsdemux pushes a PES buffer when either:
- The stated PES length is reached (`current_size == expected_size`), or
- The next packet's `payload_unit_start_indicator` is set (next PES starts)

mpegtsmux writes video PES packets with `length = 0` (unbounded), so tsdemux
always waits for the next frame. Mirrored frames are 3-16 KB, well below the
64 KB a PES length could state. Writing our own TS muxer with actual PES lengths
would remove one frame period (~33 ms at 30 fps) of delay. This is also what the
single IDR after a rotation needs to display immediately.

### TV processing

What the TV's panel processing adds (scaler, Game mode, TruMotion) after the
capture point can't be measured this way. Game picture mode with TruMotion off
would reduce it.

### Latency test harness

```sh
RPIPLAY_LATENCY_TEST=1 ./webos-build/tv-run.sh
```

Makes RPiPlay serve `app/clock.html` at `http://<tv>:7737/clock`. The player app
overlays the same clock (synced from `/time`). Mirror the clock page from the
phone, then capture the TV's display:

```sh
luna-send -n 1 luna://com.webos.service.tv.capture/executeOneShot \
  '{"method":"SCREEN","format":"JPEG","width":1920,"height":1080,"path":"/tmp/lat.jpg"}'
```

Each capture shows both clocks; the difference is the delay. Use
`scratchpad/latency-capture.sh` to automate multiple captures.

`RPIPLAY_NO_AUDIO=1` compares video-only latency.

---

## Running on other LG TVs

### Target audience

TVs without built-in AirPlay 2: 2014-2018 models (webOS 1.x-4.0). AirPlay 2
arrived with webOS 4.5 and was back-ported to some webOS 4.0 models.

### Root availability

| webOS version | Root method | Status |
|---------------|------------|--------|
| 3.5-4.x | DejaVuln / faultmanager (throwaway96) | Works |
| 1.x-3.4 | Hardware NVM mod only | Software root dead since early 2024 |
| 5.x+ | Various, including Homebrew Channel | AirPlay 2 built in (not our audience) |

Requiring root excludes most of the target audience. **Developer mode (no root)
is essential.**

### Developer mode

Native binaries run via:
```
/usr/bin/jailer -t native_devmode -i <id> -p <appdir> <binary>
```

moonlight-tv is a native app installable without root on webOS 1.x-5.x+.
Dev mode sessions last 1000 hours and need renewing from the Dev Mode app.

### Portable video path: StarfishMediaAPIs

The `miracast://` path is LG-internal and unverified on any other webOS version.
The portable low-latency path (used by moonlight-tv and Kodi) feeds H.264
directly into LG's player library:

- **StarfishMediaAPIs** (`libplayerAPIs`): `Load`/`Feed`, BUFFERSTREAM with
  `contentsType WEBRTC/LIVE`, `lowDelayMode`
- Two ABI variants: "acb" for webOS 2-4.x, "webos5" for 5+
  (see `github.com/webosbrew/samples`, `media/smp`)
- **LGNC** (`liblgncopenapi`): alternative on webOS 1-4

This eliminates the MPEG-TS/RTP/UDP layer, the web app, luna-send, and the TV's
GStreamer entirely. It's a direct H.264 → hardware decoder path.

### Toolchain

The community `webosbrew/native-toolchain` (`arm-webos-linux-gnueabi`, Cortex-A9
softfp, patched glibc 2.12 + polyfills, kernel headers 3.10) produces one ARMv7
binary that covers webOS 1.x through 26. All LG TVs have 32-bit ARM userland,
even 64-bit SoCs. This replaces our Docker toolchain linked against glibc 2.19.

### Volume without root

Non-root apps may not call `com.webos.audio` setVolume/getVolume. The LAN
WebSocket API works everywhere:

```
ws://<tv>:3000    (one-time pairing prompt on first connect)
ssap://audio/setVolume   {"volume": N}
ssap://audio/getVolume
```

### Rooted TV boot

Homebrew Channel runs `/var/lib/webosbrew/init.d` scripts as root at boot
(reliable from webOS 4.5; older needs HBChannel's Aug 2026 autostart fix or
`webosbrew-autostart`). hyperion.ng-webos-loader is the model for packaging a
root daemon as an HBChannel app.

### Proposed migration path

1. Prove the StarfishMediaAPIs direct-feed player on this TV from dev mode
   (webosbrew's `smp` sample)
2. If it works, make RPiPlay a native app with that output; keep Miracast as
   the rooted fallback
3. Switch to the webosbrew toolchain
4. Volume via the WebSocket API

---

## Debugging tips

### LG's media pipeline logs

The Miracast plugin logs as context `miracast-player` (umediaserver as `ums.*`),
but PmLogDaemon's whitelist filters them out. To read them:

```sh
stop PmLogDaemon
busybox syslogd -O /tmp/media.log -S 1024
# ... do the test ...
start PmLogDaemon
cat /tmp/media.log | grep -E 'miracast|ums'
```

A reboot also restores PmLogDaemon. The kernel log (`dmesg`) shows hardware
decoder init (`MApi_VDEC_EX_V2_Init`) and video-plane setup.

### RPiPlay log

`/tmp/rpiplay.log` on the TV. Key messages:
- `[webos-app] playing` — picture is visible
- `[webos-app] loadeddata` — pipeline loaded, video play started
- `picture N ms after replay` — successful GOP replay, N is time to "playing"
- `no picture` — replay failed, will retry or restart pipeline
- `prime: N frames, M bytes` — GOP replay size

### Display capture

```sh
luna-send -n 1 luna://com.webos.service.tv.capture/executeOneShot \
  '{"method":"SCREEN","format":"JPEG","width":1920,"height":1080,"path":"/tmp/cap.jpg"}'
```

`SCREEN` captures the composited output (app overlay + video). `VIDEO` captures
only the video plane but fails with "No Signal" when no stream is active.

### H.264 dump

```sh
RPIPLAY_DUMP_DIR=/tmp/rpdump ./webos-build/tv-run.sh
```

Saves the H.264 NALUs sent after each rotation. Useful for verifying IDR
placement and replay content.

### USR1 signal

`kill -USR1 $(pidof rpiplay)` triggers a GOP replay manually. Useful for testing
the replay path without rotating.

---

## File reference

| File | Purpose |
|------|---------|
| `video_renderer_webos.c` | Video pipeline, RTP send, app lifecycle, rotation, latency test |
| `audio_renderer_webos.c` | AAC-ELD→LC transcode, pacing, volume curve |
| `webos_av.h` | Shared audio/video interface (push_audio, session_end) |
| `dnssd_mdns.c` | Standalone mDNS for AirPlay discovery |
| `glibc_compat.c` | stat/atexit/getentropy/explicit_bzero shims (insurance) |
| `build.sh` | Docker build script |
| `Dockerfile` | Cross-compile environment (OpenSSL, libplist, fdk-aac, stubs) |
| `tv-run.sh` | Deploy and restart on the TV |
| `rpiplay-start.sh` | Boot supervisor (respawn loop, TV name lookup) |
| `app/index.html` | Player web app (Miracast video, event reporting, clock overlay) |
| `app/clock.html` | Clock page for the latency test (phone mirrors this) |
| `gst_preload.c` | Unused (was an early LD_PRELOAD experiment) |
| `gop_probe.c` | GOP layer inspector (debug tool) |
| `gop_transparent.c` | GOP alpha/colourkey setter (debug tool) |
| `TODO.md` | Current status, open issues, research |
