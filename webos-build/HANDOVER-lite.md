# RPiPlay webOS Port — Handover (light)

Port RPiPlay (open-source AirPlay mirroring server) to run natively on an LG 49UF850V TV (webOS 2.2.3, ARMv7l soft-float, kernel 3.10.23, glibc 2.19). Goal: iPhone screen mirroring works directly on the TV, no external hardware.

Full technical detail lives in HANDOVER.md; this is the paste-friendly summary.

## Status

**Working:** cross-compile toolchain (Docker, arm-linux-gnueabi, soft-float); binary runs on the TV; GStreamer + mvsink pipeline initialises; iPhone discovers the TV via mDNS and connects; AirPlay handshake completes; H.264 frames received and decoded by omxh264dec; audio pipeline (AAC-ELD, omxaacdec → pulsesink) set up; a transparent webOS app (`com.rpiplay.airplay`) is installed; an LD_PRELOAD wrapper overrides a few GStreamer-process flags; foreground notification via luna-send works.

**The one unsolved problem:** the decoded video is not visible. It renders into the MVOP (video) hardware plane via mvsink, but the GOP (graphics/UI) plane above it is fully opaque, so the picture plays out of sight underneath.

## Display stack (MStar SoC)

Two hardware layers are composited to the panel:

1. **GOP (graphics)** — where the Wayland compositor (`surface-manager-starfish`) draws the UI framebuffers (GWINs). Opaque; always on top.
2. **MVOP (video)** — the hardware video plane that mvsink writes to. Always underneath.

Commercial TV apps show video because the media server (`umediaserver`) tells the compositor to make the relevant surface region transparent over the video. Our standalone GStreamer process isn't part of that orchestration, so nothing punches the hole.

## The "DO NOTHING" design

When mvsink initialises, it calls `MApi_XC_Set_GstreamerProcess(TRUE)` (via `libmsil_lm15u.so`). All `msil_scaler_*` functions then check that flag and return immediately ("DO NOTHING") — by design, because the media server normally handles display-path setup externally. Forcing them to execute crashes our process (the Utopia/XC module state the media server would have initialised is absent; a forced call faults inside libkadaptor near `UtopiaInstanceGetPrivate`).

Tried so far: LD_PRELOAD wrapper over the process-flag getters (partial), redirected scaler calls (crash), direct MApi window/MVOP enable calls from the renderer (no crash, no visible change).

## Promising next directions

1. **Media-server registration (best bet).** Apps use `luna://com.webos.media/load` (+ `notifyForeground`) to register the pipeline with the media server, which then orchestrates the compositor to open a transparent window over the video region. We already send `notifyForeground`; try the full load/attach flow.
2. **GOP transparency tooling.** `gop_probe` / `gop_transparent` can read GWIN state and set blending/alpha/colour-key. Not yet tested against a live stream. Caveats: killing all GWINs removes the UI; alpha needs to be localised to the app window.
3. **Precedent: the "Screen Share" app** (`com.webos.app.miracast`) is just a web app with `"transparent": true` in appinfo.json — an app-window that lets the video plane show through underneath. The same trick should work for an AirPlay shell app when it is the foreground window; the input-menu listing itself is service-driven (look in `/usr/palm/services` and configd layers).
4. **Wayland route** — render through the compositor with a wayland surface if `waylandsink` exists on GStreamer 1.2.4 (currently no gstreamer wayland plugin found on the TV — likely needs the starfish EGL client lib).
5. **Framebuffer direct write** — appsink + `/dev/fb0`; simple, no HW scaling.
6. **Library binary edits** at the DO NOTHING checks (last resort).

## Environment facts

- TV: LG 49UF850V, webOS 2.2.3, MStar "monaco/lm15u", ARMv7l soft-float, kernel 3.10.23, glibc 2.19, GStreamer 1.2.4 (gst-omx + MStar `libOMX.MStar.so` work; mvsink/masink/mfc plugins present).
- Access: dev-mode SSH (port 9922); admin access via the `towelroot` helper (a 2014 futex-requeue kernel issue fixed long upstream; used here as a setup tool) which creates `/tmp/rootsh` (re-run after each reboot; /tmp clears).
- Key processes: `surface-manager-starfish` (the compositor — never kill), `WebAppMgr -platform wayland`, `umediaserver`.
- Key libraries: `libkadaptor.so.1.0.0` (MStar SDK monolith; exports `MApi_GOP_*`, `MApi_XC_*`, `MDrv_MVOP_*`), `libmsil_lm15u.so.1` (scaler wrappers + the DO NOTHING guards), `libgstmvsink.so` (links libmsil).
- Build: `./webos-build/build.sh` (Docker). Custom CRT + TV sysroot glibc 2.19, `-mfloat-abi=soft -fno-stack-protector`, stub-linked GStreamer.
- Pipeline: `appsrc ! h264parse ! omxh264dec ! mvsink` (video), `appsrc ! aacparse ! omxaacdec ! audioconvert ! pulsesink` (audio). Both confirmed running end to end; frames decoded.
- Constraint: transparency applies per app surface via the compositor — the shell app must be the foreground window for the "show-through" behaviour, which matches the launch-on-session-start design (`com.webos.applicationManager/launch`).

Full details, crash offsets, function tables, deploy commands: see HANDOVER.md.
