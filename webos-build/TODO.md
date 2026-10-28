# webOS port: status and remaining work

Target: LG 49UF850V, webOS 2.2.3 (MStar LM15U, ARMv7 soft-float, glibc 2.19,
GStreamer 1.2.4). RPiPlay runs as root outside the dev-mode jail and streams
RTP/MPEG-TS to 127.0.0.1:5000, which the `com.rpiplay.airplay` web app plays
through the TV's Miracast media pipeline (`miracast://` video source).

## Working

- Mirroring with sound (AAC-ELD -> AAC-LC re-encode, muxed into the TS)
- Latency stays flat (arrival-time timestamps; see `stamp()`)
- AirPlay name follows the TV's network name (`tv-run.sh`)
- Rotation: the TV player can't follow a picture size change, so the app is
  closed and relaunched (always as 1920x1080) and the cached GOP is replayed

## Open issues

1. **Rotation can still go black** (in testing as of 2026-10-28)
   - Rotating 3-4 times quickly could leave an unrecoverable black screen;
     a further rotation 10-15 s later, or reconnecting, fixed it.
   - Current theory: the TV has one video decoder and the old media pipeline
     process (`starfish-media-pipeline`) takes seconds to release it; a new
     pipeline started meanwhile stays black. Latest build waits for that
     process to exit before relaunching (`close_app`, 5 s timeout) and
     serialises restarts (`restart_pending`). Check `/tmp/rpiplay.log` for
     "old pipeline gone after N ms" when testing.
   - Ruled out: dropped UDP packets (RcvbufErrors 0), replay not starting on
     an IDR, settle time granularity. Pacing the GOP replay made it worse.
   - Landscape replays are ~1-1.5 MB in one burst; still a suspect.
   - If waiting doesn't fix it: have the app report `<video>` events back
     (e.g. XHR to a small listener in RPiPlay) so RPiPlay can detect a black
     restart and retry automatically.

2. **Performance** (next after rotation)
   - Measure end-to-end latency and CPU; check `displayQueueSize`, jitter
     buffer latency in the Miracast pipeline, GOP replay cost.
   - `TV_SETTLE_MS` (1000) and restart time could be tuned down.

3. **TV volume control**
   - The iPhone volume slider currently applies software gain only.
   - Map it to the TV's master volume (`luna://com.webos.audio/...`), but
     guard against iOS sending full volume on connect.

4. **Packaging for others**
   - Clean repo layout, build instructions, prebuilt ipk + binary.
   - Startup at boot (currently `tv-run.sh` from a Mac).
   - Remove leftovers: `gst_preload.c` unused; `HANDOVER-lite.md` partly wrong.

5. **Root and non-root (developer mode) support**
   - Root is needed today only because `luna-send` (app launch/close) doesn't
     work inside the dev-mode jail.
   - Investigate: a dev-mode app with a native/JS service can call luna APIs
     with the app's own permissions; RPiPlay could run as that service, or
     the app could launch/close itself on a signal from RPiPlay. Also check
     whether the jail allows binding the AirPlay ports and mDNS (5353).
