# Latency measurement methods

Everything measured on the 49UF850V uses one principle: put two clocks on
the screen that are both derived from the **TV's own clock domain**, capture
the screen, and read the difference. Using the TV for both ends kills clock
sync as an error source entirely, and the capture gives a permanent,
inspectable record (unlike a photo of a screen).

## The capture: `com.webos.service.tv.capture/executeOneShot`

    luna-send -n 1 -w 5000 luna://com.webos.service.tv.capture/executeOneShot \
      '{"method":"SCREEN","format":"JPEG","width":1920,"height":1080,"path":"/tmp/shot.jpg"}'

Works on webOS 2.2 without root tricks (call it through
`/proc/1/root/bin/busybox chroot /proc/1/root`). One shot takes ~0.2 s and
the JPEG shows the **composited picture including the hardware video
plane** - verified: mirrored video and UI overlays both appear. Reads back
over ssh (`cat /tmp/shot.jpg`).

Consequence: latency = (TV-side clock shown by the overlay) - (clock shown
by the mirrored content), read off one image. No stopwatch maths, no phone
camera.

## Method A - the shipping path (miracast-udp), PROVEN

`RPIPLAY_LATENCY_TEST=1 ./webos-build/tv-run.sh`:

- RPiPlay serves `app/clock.html` on `http://<tv-ip>:7737/clock`; the page
  fetches the TV's wall clock from `http://<tv-ip>:7737/time` and renders
  it large.
- The phone opens the page and AirPlay-mirrors it, so the *content* shown
  is the TV's own clock, delayed by the whole path: phone render -> H.264
  encode -> Wi-Fi -> RPiPlay -> TS mux -> TV web player -> plane -> panel.
- The player web app overlays its own copy of the clock (RPiPlay's, read
  from `/time`) on top, undelayed.
- One `executeOneShot` JPEG per sample; the two clocks in the frame differ
  by the end-to-end latency.

Baseline numbers (2026-10-28, clock-overlay method): **~50 ms with audio**
(41-59 over six captures), **~24 ms video only** (16-30,
`RPIPLAY_NO_AUDIO=1`). Audio costs ~27 ms (mux wait + TV A/V sync, see
TODO.md).

## Method B - SMP direct feed (PoC), IN PROGRESS

Same capture, two differences: the content clock is a file, and there is no
web app to supply the overlay.

1. **Content**: `output/smp-poc/bin/clock.h264` (+ silent `clock.aac`) -
   1080p30, one giant `hh:mm:ss.mmm` counter, red bar at top for frame 0
   identification, generated in the builder container with
   `-tune zerolatency`, all-keyframe-interval 30, no B frames. The SMP
   sample paces `Feed()` on wall clock, so a frame carrying time T is
   presented to the pipeline at wall T: what the screen shows is
   `now - (pipeline + panel delay)`, exactly what we want to measure.
2. **The overlay**: patched sample draws TV wall clock
   (`gettimeofday`) through SDL every frame. This is necessary because the
   direct feed runs as a native jail app with no webapp UI - the
   transparent SDL window *is* the graphics layer, so it is also what
   `executeOneShot` composites over the video plane. Patch lives in
   `output/smp-poc/` (GPL: study/patch, not copied into the MIT tree).
3. **Launch** (the app manager on 2.x cannot launch native dev apps -
   `jailer` is the working path on webOS 2.2, and it gives stdout):

       XDG_RUNTIME_DIR=/tmp/xdg /usr/bin/jailer -t native_devmode \
         -i org.webosbrew.sample.media.smp.acb \
         -p /media/developer/apps/usr/palm/applications/org.webosbrew.sample.media.smp.acb \
         <app-dir>/media-smp-acb-webos2 \
         '{"@system_native_app":true,"nid":"org.webosbrew.sample.media.smp.acb"}' \
         --video clock.h264 --audio clock.aac --fps 30

   (`output/smp-poc/run-clock-tv.sh` wraps this with logging.)
4. **Sampling**: burst 5-10 shots across the 30 s playback; report median
   and spread, same presentation as Method A.

Gate (from TODO.md): switch the release default only if this **measurably
beats** ~50 ms / ~24 ms. Parity is not a reason to rewrite.

### Error analysis / caveats

- The shot takes ~0.2 s and its JPEG is written after capture, but this
  does not bias the reading: both clocks are captured at the same
  instantaneous moment and the difference is taken within one frame.
- Panel processing *after* the plane (TruMotion etc.) is invisible to
  `executeOneShot` on both methods equally; Game mode would shift both
  numbers the same way.
- `/proc/uptime` is the only millisecond clock in busybox (`date +%N`
  prints literally) - useful for burst bookkeeping, not needed for the
  in-frame difference.
- The sample's log lines go to stderr; under jailer the app's own file
  redirect lands in the jail's private `/tmp`. LG stack noise goes to
  stdout - that is what makes jailer + `> file 2>&1` usable at all.
- Boot-to-picture cost is separate from steady-state latency: the webOS 2
  sample needs ~0.5 s between `Load()` and `load completed` (resource
  acquisition), which matters for session start, not for mirroring feel.

## Method C - phone camera (original), RETIRED

Before `executeOneShot` was known the clocks were framed with a phone
camera and filmed. Still valid, strictly worse: shutter timing quantises
samples, one capture per photo, no machine-readable record. Kept only
because the original 50/24 ms figures were taken with a hybrid of this and
Method A.
