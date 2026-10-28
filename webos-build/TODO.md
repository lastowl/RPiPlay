# webOS port: status and remaining work

Target: LG 49UF850V, webOS 2.2.3 (MStar LM15U, ARMv7 soft-float, glibc 2.19,
GStreamer 1.2.4). RPiPlay runs as root outside the dev-mode jail and streams
RTP/MPEG-TS to 127.0.0.1:5000, which the `com.rpiplay.airplay` web app plays
through the TV's Miracast media pipeline (`miracast://` video source).

## Working

- Mirroring with sound (AAC-ELD -> AAC-LC re-encode, muxed into the TS)
- Latency stays flat (arrival-time timestamps; see `stamp()`)
- AirPlay name follows the TV's network name (`tv-run.sh`)
- Auto-start at boot: `boot-root.sh` launches `rpiplay-start.sh` after sshd;
  RPiPlay comes up ~3-4 min after power-on and respawns if it crashes
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

## Open issues

1. **Performance**
   - Measure end-to-end latency and CPU; check `displayQueueSize`, jitter
     buffer latency in the Miracast pipeline, GOP replay cost.
   - `TV_SETTLE_MS` (1000) and restart time could be tuned down.

2. **Packaging for others**
   - Clean repo layout, build instructions, prebuilt ipk + binary.
   - ~~Startup at boot~~ (done — `boot-root.sh` → `rpiplay-start.sh`).
   - Remove leftovers: `gst_preload.c` unused; `HANDOVER-lite.md` partly wrong.

3. **Root and non-root (developer mode) support**
   - Root is needed today only because `luna-send` (app launch/close) doesn't
     work inside the dev-mode jail.
   - Investigate: a dev-mode app with a native/JS service can call luna APIs
     with the app's own permissions; RPiPlay could run as that service, or
     the app could launch/close itself on a signal from RPiPlay. Also check
     whether the jail allows binding the AirPlay ports and mDNS (5353).

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
