# webOS port: status and remaining work

Target: LG 49UF850V, webOS 2.2.3 (MStar LM15U, ARMv7 soft-float, glibc 2.19,
GStreamer 1.2.4). RPiPlay runs as root outside the dev-mode jail and streams
RTP/MPEG-TS to 127.0.0.1:5000, which the `com.rpiplay.airplay` web app plays
through the TV's Miracast media pipeline (`miracast://` video source).

## Working

- Mirroring with sound (AAC-ELD -> AAC-LC re-encode, muxed into the TS)
- Latency stays flat (arrival-time timestamps; see `stamp()`)
- AirPlay name follows the TV's network name (`tv-run.sh`)
- Auto-start at boot: `startup.sh` (hooked into `start-devmode.sh` by upstart)
  brings up root ssh on :22 (pubkey, hands-off within ~1-3 min of power-on)
  and the RPiPlay supervisor; the TV also wakes to a Wake-on-LAN packet
  (`wake-tv.sh`), and `tv-run.sh`/`install-ipk.sh` do that automatically
- Rotation: the TV player can't follow a picture size change, so the app is
  relaunched (its `webOSRelaunch` handler replaces the `<video>`, always as
  1920x1080) and the cached GOP is replayed. See "Rotation notes" below.
- TV volume: the iPhone's volume drives the TV's master volume
  (`com.webos.audio/setVolume`, `setMuted`) along a curve covering 0-100
  (`tv_volume_curve` in `audio_renderer_webos.c`). It is applied in the
  SET_PARAMETER handler, since a mirroring session has no audio stream
  until something plays, and iOS's GET_PARAMETER at connect gets the TV's
  place on the curve (`get_volume`).
  - The slider doesn't follow the TV remote (that would need DACP), so the
    two can drift apart; pushing the slider to either end lines them up.

## Performance (measured 2026-10-28)

- CPU: RPiPlay ~20% of one core (the TV has four), the TV's media pipeline
  ~29%. Inside RPiPlay no thread is above 4%. Not a concern.
- Delay from the phone's screen to the TV's composited picture, measured
  with the latency test below: **~50 ms with audio** (41-59 over six
  captures), **~24 ms video only** (16-30). Audio costs ~27 ms: mpegtsmux
  lets a video frame out only once an audio frame (one every 23 ms) is
  queued too, and the TV's A/V sync then holds video for the audio path's
  latency.
- The TV's demuxer holds each frame until the next one starts, one more
  frame period: mpegtsmux writes video PES packets with length 0, while
  tsdemux (1.2.4, `gst_ts_demux_handle_packet`) pushes a PES as soon as its
  stated length is complete. Mirrored frames are 3-16 KB, far below the
  64 KB a PES length can state. Our own TS muxer would remove this and the
  audio wait, and is also what the lone IDR after a rotation needs.
- What the TV's picture processing adds after the capture point can't be
  measured this way; Game picture mode / TruMotion off would reduce it.
- `TV_SETTLE_MS` (1000) and restart time could still be tuned down.

### Latency test

`RPIPLAY_LATENCY_TEST=1 ./webos-build/tv-run.sh` makes RPiPlay serve
`app/clock.html` at `http://<tv>:7737/clock` and the player app overlay the
same clock (RPiPlay's, read from `/time`). Mirror that page from the phone,
then capture the TV's display: `luna://com.webos.service.tv.capture/
executeOneShot` with `{"method":"SCREEN","format":"JPEG","width":1920,
"height":1080,"path":...}` (~0.2 s each). Each capture shows both clocks;
the difference is the delay. `RPIPLAY_NO_AUDIO=1` streams video only.

## Open issues

1. **Running on other LG TVs** (research 2026-10-28)
   - Audience: TVs without built-in AirPlay 2, i.e. 2014-2018 sets (webOS
     1.x-4.0; AirPlay 2 came with webOS 4.5 and was back-ported to some
     webOS 4.0 models). Root today: webOS 3.5-4.x via DejaVuln/faultmanager
     (throwaway96); webOS 1.x-3.4 have had no software root since early 2024
     (hardware NVM mod only); RootMyTV never covered <3.4. So requiring root
     excludes most of the audience: developer mode (no root) is essential.
   - Dev mode runs native binaries: `/usr/bin/jailer -t native_devmode -i
     <id> -p <appdir> <binary>` (webosbrew.org/pages/native-development);
     moonlight-tv is a native app installable without root on webOS 1.x-5.x+.
     Dev mode sessions last 1000 h and need renewing from the Dev Mode app.
   - The `miracast://` web-app path is LG-internal and unverified on any
     other version (LG documents only HTTP/HLS transports). The portable
     low-latency path used by moonlight-tv and Kodi feeds the H.264
     elementary stream straight into LG's player library
     (`libplayerAPIs`, StarfishMediaAPIs `Load`/`Feed`, BUFFERSTREAM with
     `contentsType WEBRTC/LIVE`, `lowDelayMode`): two ABI variants, "acb"
     for webOS 2-4.x and "webos5" for 5+ (github.com/webosbrew/samples,
     media/smp), plus LGNC (`liblgncopenapi`) on webOS 1-4. That needs no
     MPEG-TS/RTP/UDP, no web app, no luna-send and none of the TV's
     GStreamer (1.2.4 here, 1.14 on webOS 5, 1.24 on webOS 25).
   - Toolchain: the community builds with buildroot-nc4 /
     `webosbrew/native-toolchain` (`arm-webos-linux-gnueabi`, Cortex-A9
     softfp, a patched glibc 2.12 plus polyfills, kernel headers 3.10) and
     one armv7 binary covers webOS 1.x to 26; userland is 32-bit ARM on
     every LG TV, including 64-bit SoCs. It replaces our Docker toolchain
     linked against glibc 2.19 pulled from this TV.
   - Volume without root: apps may only call `com.webos.audio`
     setMuted/volumeUp/volumeDown; `setVolume`/`getVolume` as used here may
     be refused. The LAN WebSocket API (`ws://<tv>:3000`, one pairing
     prompt, `ssap://audio/setVolume`, `getVolume`) works on every version.
   - Rooted TVs: Homebrew Channel runs `/var/lib/webosbrew/init.d` scripts
     as root at boot (reliable from webOS 4.5; older needs HBChannel's
     Aug 2026 autostart fix or `webosbrew-autostart`); hyperion.ng-webos-
     loader is the model for a root daemon packaged as an HBChannel app.
   - ~~switch to the webosbrew toolchain~~ done 2026-10-30 (awaiting TV
     test): build.sh/Dockerfile now use the pinned buildroot-nc4
     `2026.08-webos` SDK (glibc 2.12, host-arch-aware). Verified ELF:
     soft-float (unchanged ABI - the SDK forces it regardless of flags),
     max symbol requirement GLIBC_2.4/2.7 (was 2.19-specific), no
     libstdc++/libgcc_s needed. tv-sysroot + crt1.S + manual ld all
     deleted; the ci-sysroot branch keeps the old blobs for reference.
     Ship as v1.2.0 once the TV run passes.
   - Proposed order: (1) prove the direct-feed player on this TV from dev
     mode with webosbrew's smp sample; (2) if it works, make RPiPlay a
     native app with that output, keep the Miracast path as the rooted
     fallback; (3) switch to the webosbrew toolchain; (4) volume via the
     WebSocket API.
   - Prior art found 2026-10-30: **danlflh/uxplay-webos** (GPL-3.0, v0.1.0
     Sep 2026) is a UxPlay-based (AirPlay 2, PIN) receiver for rooted webOS
     TVs: native app, SMP/ACB direct feed, SDL2, FFmpeg audio, webosbrew
     toolchain — validated only on one webOS 3.9 panel, 720p/30, no boot
     autostart. We differ on: webOS 2.2, 1080p, measured ~50 ms latency,
     boot autostart, iPhone volume; it has AirPlay 2. Their docs: "Starfish's
     buffering contributes latency on this older platform; stability is
     prioritized over gaming latency" — latency unquantified.
   - Assessment: the latency is most likely their AirPlay 2 transport
     (LL-HLS-style segmenting) plus conservative queueing, not SMP itself —
     moonlight-tv streams games through the same `libplayerAPIs`
     BUFFERSTREAM/`lowDelayMode` path on the same class of panels, and Kodi
     uses it for live TV. So SMP is not a dead end, but two cautions:
     nobody has run it on webOS 2.2/LM15U, and their result is what the
     low-latency knobs look like when they are not (or cannot be) set right
     on the older "acb" ABI.
   - Gate: **only switch video paths if the direct feed beats the current
     Miracast path measurably (target < ~50 ms with audio, < ~24 ms video
     only, clock-overlay method)** — the Miracast path is working, at 1080p,
     and fully measured; parity is not a reason to rewrite. Reference:
     their repo for ACB-on-2-4.x usage (GPL: study, do not copy into MIT),
     webosbrew/samples media/smp, mariotaku/ss4s.
    - Architecture decision (2026-10-31): **keep every render path in the
      tree, switchable at runtime** — research value for others, and
      different generations may prefer different paths. Shape: a small
      video-out backend interface (miracast-udp | smp-direct | future
      lgnc), selected by `--video-out` / `RPIPLAY_VIDEO_OUT`; the SMP
      backend dlopen()s `libplayerAPIs`/`libAcbAPI`/SDL2 so one binary
      still runs everywhere (missing libs => the backend refuses to init,
      never crashes the default path). Releases default to the proven
      path (currently `miracast-udp`); the flag is documented, not
      advertised. The SMP work therefore lands as an additive backend,
      not a rewrite - and item 3 (AirPlay 2) stays orthogonal: any
      transport feeds any backend.

2. **Packaging for others**
   - ~~Prebuilt ipk + binary~~ done (v1.1.0): the ipk bundles app + binary
     (`bin/rpiplay`) + `rpiplay-start.sh`; `install-ipk.sh` installs, chmods,
     sets the boot hook and starts in one command (`package-app.sh install`
     builds and does the same).
   - ~~GitHub release carrying the prebuilt ipk~~ done 2026-10-30: CI
     (`.github/workflows/build-release.yml`) builds on tag `v<version>` and
     attaches the ipk; lastowl/RPiPlay releases page is live, INSTALL.md
     links it. Build inputs: TV sysroot on orphan branch `ci-sysroot`.
   - Clean repo layout, build instructions.
   - Remove leftovers: `gst_preload.c` unused; `HANDOVER-lite.md` partly wrong.

3. **AirPlay 2 support** (added 2026-10-31 — the long-term item)
   - We speak AirPlay 1 (Classic): fine today — iOS still mirrors over it
     and every iPhone still has it — but Apple has been routing new
     features (HEVC mirroring, spatial audio, lower-latency sessions)
     through AirPlay 2 only, so the protocol ages out eventually.
   - AirPlay 2 mirroring means: ed25519 pairing/handshake, an encrypted
     HTTP LIVE mux stream, H.264/HEVC samples already H.264-friendly for
     our pipeline. No MIT-licensed receiver library implements the AP2
     mirroring subset yet; uxplay does (GPL-3) and danlflh/uxplay-webos
     already proves it runs on webOS (webOS 3.9, 720p/30).
   - Options: (a) a companion uxplay-based receiver packaged beside the
     app (GPL stays in its own binary — same "study, don't copy" firewall
     as the SMP research); (b) implement the AP2 receive layer natively
     (large: pairing, stream, timing, and no spec); (c) track upstream
     RPiPlay for an AP2 branch. Preferred: (a) first, (c) opportunistic.
   - Synergy: whichever transport wins in item 1 (SMP direct feed), AP2
     becomes purely a receive-side project — decoding and display stay
     the same.

## Rotation notes

- Before the fix, rotating could leave a black screen until a later
  rotation or a reconnect (iOS rarely sends a new IDR, so waiting didn't help).
- The app reports `<video>`/umediaserver events to RPiPlay (port 7737,
  logged as `[webos-app]`). A replay that worked gets umediaserver
  "playing" ~0.3 s later; a black one never does.
- From `libpf-miracastplugin.so`: the plugin drains its UDP socket only
  when a pipeline resumes from pause, not on a new pipeline's first play.
  Sending only SPS/PPS + IDR and holding the rest until "playing" never
  showed anything (the demuxer holds a frame until the next one starts).
- Cause (from LG's logs): a new pipeline adds its tsdemux pads and starts
  the hardware decoder (`MApi_VDEC_EX_V2_Init` in the kernel log) only when
  the first data arrives, and a replay burst sent then shows nothing. Every
  landscape restart and some portrait ones failed this way; a second replay
  worked every time. The TV socket's drop count stayed 0 (160 KB default
  buffer, not the cause).
- Fix: first replay 300 ms after the new pipeline's socket appears (and the
  old one has closed), a second one 1 s later if no "playing" arrives, then
  a pipeline restart after another 2 s. RTP is sent by RPiPlay itself,
  paced to 6 MB/s (`send_rtp`). `/tmp/rpiplay.log` shows "picture N ms
  after replay" and "no picture".
- Ruled out earlier: replay not starting on an IDR, settle time
  granularity, closing the app instead of relaunching, per-frame paced
  replay (4 ms/frame, seemed worse).
- LG's logs: the plugin logs as context `miracast-player` (umediaserver as
  `ums.*`), filtered out by PmLogDaemon's whitelist. To read them, stop
  PmLogDaemon and run busybox syslogd to a file for the test, then
  `start PmLogDaemon` (a reboot also restores it).
