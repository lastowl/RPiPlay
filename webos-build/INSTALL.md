# Installing RPiPlay on an LG webOS TV

AirPlay mirroring on your LG TV — no Apple TV, no Raspberry Pi.

**Tested on:** LG 49UF850V (webOS 2.2.3). Should work on other rooted webOS
2.x TVs. See [Compatibility](#compatibility) for other versions.

## What you need

- A rooted LG webOS TV with SSH access
- A Mac or Linux machine on the same network (for building and deploying)
- Docker (for the cross-compiler)

### Rooting your TV

For webOS 2.x TVs, see
[lg-webos2-root](https://github.com/lastowl/lg-webos2-root) — persistent root
via CVE-2014-3153, tested on the 49UF850V.

For webOS 3.5-4.x, see [RootMyTV](https://rootmy.tv) /
[DejaVuln](https://github.com/throwaway96/dejavuln-autoroot).

You need:
- Root SSH access (port 22)
- An SSH key pair for the TV (e.g. `~/.ssh/uf850v_webos`)

## Quick start

```sh
# Clone the repo
git clone -b webos-port https://github.com/lastowl/RPiPlay.git
cd RPiPlay

# Build (first run pulls/builds the Docker image — takes a few minutes)
./webos-build/build.sh

# Deploy and start on the TV
./webos-build/tv-run.sh 192.168.50.99
```

Replace `192.168.50.99` with your TV's IP address.

RPiPlay will appear on your iPhone/iPad as an AirPlay receiver using the TV's
network name. Open Control Centre, tap Screen Mirroring, and select it.

## Install the prebuilt package

If you only want to run it (no Docker, no building): take the
`com.rpiplay.airplay_<version>_all.ipk` (from the GitHub releases, or build
your own with `./webos-build/build.sh && ./webos-build/package-app.sh`) and
run:

```sh
./webos-build/install-ipk.sh com.rpiplay.airplay_1.1.0_all.ipk 192.168.50.99
```

That one command installs the app and binary through the TV's package
manager, makes them executable, sets up autostart at boot and starts
RPiPlay right away. It needs the same root SSH access and key as above
(different key: pass it as a third argument).

## Setup details

### SSH key

`tv-run.sh` expects the SSH key at `~/.ssh/uf850v_webos`. To use a different
key, edit the `KEY=` line in `tv-run.sh` and `rpiplay-start.sh`.

The TV's sshd is old (dropbear), so your SSH client needs legacy algorithm
flags. `tv-run.sh` handles this, but if you SSH manually:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i ~/.ssh/uf850v_webos -p 22 root@<tv-ip>
```

### TV sysroot

The build links against the TV's glibc 2.19. You need a copy of the TV's
`/lib` in `webos-build/tv-sysroot/lib/`. The essential files:

```
libc-2.19.so  libc.so.6  ld-linux.so.3
libm-2.19.so  libm.so.6
libpthread-2.19.so  libpthread.so.0
libdl-2.19.so  libdl.so.2
```

Copy them from the TV:

```sh
mkdir -p webos-build/tv-sysroot/lib
scp -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -i ~/.ssh/uf850v_webos -P 22 \
    'root@<tv-ip>:/lib/libc-* /lib/libc.so.6 /lib/ld-linux.so.3 /lib/libm-* /lib/libm.so.6 /lib/libpthread-* /lib/libpthread.so.0 /lib/libdl-* /lib/libdl.so.2' \
    webos-build/tv-sysroot/lib/
```

These are gitignored (proprietary LG/glibc binaries).

### Install the app

The package (web app + `rpiplay` binary + start script) is built with:

```sh
./webos-build/package-app.sh              # -> webos-build/output/*.ipk
./webos-build/package-app.sh install <tv-ip>   # build and install in one go
```

Installing through the package manager (rather than copying files over the
app dir) makes the application manager re-read appinfo.json. `tv-run.sh`
deploys the same layout directly, without building an ipk.

### Auto-start at boot

`install-ipk.sh` and `tv-run.sh` keep a line in the TV's
`/media/developer/persist/boot-root.sh` that starts RPiPlay when the TV
powers on, so this normally needs no setup. To do it by hand:

```sh
ssh <tv> # (with the flags above)
cat >> /media/developer/persist/boot-root.sh << 'BOOT'
/media/developer/apps/usr/palm/applications/com.rpiplay.airplay/rpiplay-start.sh &
BOOT
chmod 755 /media/developer/persist/boot-root.sh
```

`rpiplay-start.sh` (installed by the ipk) handles the respawn loop and
reads the TV's name for AirPlay discovery.

## Usage

### Basic

```sh
./webos-build/tv-run.sh <tv-ip>           # deploy and start
./webos-build/tv-run.sh <tv-ip> stop      # stop RPiPlay
```

### Options (environment variables)

```sh
# Video only (lower latency, no audio)
RPIPLAY_NO_AUDIO=1 ./webos-build/tv-run.sh <tv-ip>

# Latency test mode (serves a clock page for measurement)
RPIPLAY_LATENCY_TEST=1 ./webos-build/tv-run.sh <tv-ip>

# Save H.264 after each rotation (debugging)
RPIPLAY_DUMP_DIR=/tmp/rpdump ./webos-build/tv-run.sh <tv-ip>

# Pass extra RPiPlay flags
RPIPLAY_ARGS="-d" ./webos-build/tv-run.sh <tv-ip>
```

### Logs

On the TV: `/tmp/rpiplay.log`

```sh
ssh <tv> 'tail -f /tmp/rpiplay.log'
```

## Compatibility

| webOS | Root method | Status |
|-------|-----------|--------|
| 2.x | [lg-webos2-root](https://github.com/lastowl/lg-webos2-root) | **Tested** (49UF850V) |
| 3.5-4.x | DejaVuln / RootMyTV | Should work (untested) |
| 1.x-3.4 | Hardware NVM mod only | No software root available |
| 4.5+ | Various | Has AirPlay 2 built in |

The current video path uses the TV's Miracast pipeline (`miracast://`), which is
undocumented and may behave differently on other webOS versions. A more portable
path using StarfishMediaAPIs (direct H.264 feed) is planned — see
[WEBOS-PORT.md](WEBOS-PORT.md) for details.

## Performance

- **Latency:** ~50 ms with audio, ~24 ms video-only
- **CPU:** ~20% of one core (RPiPlay) + ~29% (TV pipeline) — the TV has four cores
- Volume control: iPhone slider drives TV volume (0-100, custom curve)
- Rotation: handled automatically (app relaunch + GOP replay, ~0.3-1 s)

## Related projects

- **[danlflh/uxplay-webos](https://github.com/danlflh/uxplay-webos)** — an
  AirPlay **2** receiver for rooted webOS TVs (3.9+), built on UxPlay: PIN
  pairing, native app, direct hardware-decoder feed. If you want AirPlay 2
  pairing/PIN or your TV runs webOS 3.5+, that is the one to use; ours focuses
  on older sets (tested on webOS 2.2) with AirPlay 1 mirroring, 1080p,
  measured low latency and boot autostart. The two projects are complementary
  — worth a star.

## Technical documentation

See [WEBOS-PORT.md](WEBOS-PORT.md) for the full technical deep-dive: how the
Miracast trick works, rotation handling, audio transcode, build system
internals, latency measurements, and cross-TV research.
