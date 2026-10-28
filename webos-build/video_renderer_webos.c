/*
 * Video renderer for LG webOS TVs
 * Based on RPiPlay's video_renderer_gstreamer.c
 *
 * The H.264 stream is remuxed to MPEG-TS over RTP and sent to a local UDP
 * port. The com.rpiplay.airplay web app plays that port through the TV's
 * built-in Miracast media pipeline (a <video> with a "miracast://" source),
 * so umediaserver and the compositor own decoding and the video plane,
 * exactly as for the stock Screen Share app.
 *
 * Must run in the real rootfs (not the dev-mode jail) for luna-send to work.
 */

#include "video_renderer.h"
#include "webos_av.h"
#include <assert.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define APP_ID "com.rpiplay.airplay"
#define UDP_PORT 5000

/* iOS sends an IDR only when mirroring starts (or the picture changes a
 * lot), so the TV decoder must be running before the stream begins. On a new
 * connection we launch the app and hold the connection (conn_init runs on
 * the RAOP HTTP thread, so iOS just waits) until the TV pipeline has bound
 * its port and had TV_SETTLE_MS to start up.
 *
 * As a fallback for streams that start anyway, the codec config and every
 * frame since the last IDR are kept and replayed ("primed") when the TV
 * becomes ready. A replay is repeated only if the TV shows nothing: with an
 * audio track the TV plays every replayed frame in A/V sync mode, so repeated
 * replays queue up seconds of stale video. */
#define GOP_MAX_BUFFERS 600
#define GOP_MAX_BYTES (8 * 1024 * 1024)
#define TV_SETTLE_MS 1000
#define REPLAY_SETTLE_MS 300       /* the same for a restarted pipeline */
#define TV_READY_POLL_MS 100
#define TV_READY_TIMEOUT_SECONDS 10
#define STALE_SOCKET_WAIT_MS 3000
#define STATS_SECONDS 5
#define APP_EVENT_PORT 7737        /* must match EVENT_PORT in app/index.html */
/* After a replay the TV reports videoInfo/"playing" within ~0.3 s when it
 * shows the picture. The first replay to a new pipeline often shows nothing
 * (always for landscape): it is what makes the pipeline add its demuxer pads
 * and start the hardware decoder (seen in LG's logs), and the burst sent
 * meanwhile is lost. A second replay then works, so retry after
 * REPLAY_RETRY_MS, and restart the pipeline if that fails too. */
#define REPLAY_RETRY_MS 1000
#define PICTURE_TIMEOUT_MS 2000
#define REPLAYS_BEFORE_RESTART 1
#define PIPELINE_EXIT_TIMEOUT_MS 5000
/* The TV pipeline reads UDP_PORT with the kernel's default receive buffer
 * (net.core.rmem_default = rmem_max = 160 KB, roughly 70 RTP packets), and
 * replays are 1-2 MB. RTP packets go out at up to SEND_RATE bytes/s, in
 * bursts of at most SEND_BURST, so a reader stall can't overflow it (no drops
 * have been seen either way). */
#define SEND_RATE (6 * 1024 * 1024)
#define SEND_BURST (48 * 1024)

static volatile sig_atomic_t prime_requested = 0;

static void usr1_handler(int sig) {
    (void)sig;
    prime_requested = 1;    /* kill -USR1 <pid> replays the GOP, for debugging */
}

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
    fprintf(stderr, "  PC = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_pc);
    fprintf(stderr, "  LR = %p\n", (void*)(unsigned long)uc->uc_mcontext.arm_lr);
    fflush(stderr);

    /* Show which libraries PC and LR fall in */
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[256];
        unsigned long pc = uc->uc_mcontext.arm_pc, lr = uc->uc_mcontext.arm_lr;
        /* Executable mappings, for a heuristic backtrace (no frame pointers) */
        struct { unsigned long start, end, off; char name[96]; } maps[128];
        int nmaps = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long start, end, off;
            char perms[8], name[96] = "";
            if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %95s", &start, &end, perms, &off, name) < 4)
                continue;
            if ((pc >= start && pc < end) || (lr >= start && lr < end))
                fprintf(stderr, ">>> %s", line);
            if (perms[2] == 'x' && nmaps < 128) {
                maps[nmaps].start = start;
                maps[nmaps].end = end;
                maps[nmaps].off = off;
                strcpy(maps[nmaps].name, name);
                nmaps++;
            }
        }
        fclose(f);
        /* Words on the stack that point into code are likely return addresses */
        unsigned long *sp = (unsigned long *)uc->uc_mcontext.arm_sp;
        fprintf(stderr, "  stack code pointers (file offsets):\n");
        for (int i = 0, shown = 0; i < 2048 && shown < 40; i++) {
            for (int m = 0; m < nmaps; m++) {
                if (sp[i] >= maps[m].start && sp[i] < maps[m].end) {
                    fprintf(stderr, "    sp+%d: %s+0x%lx\n", i * 4, maps[m].name,
                            sp[i] - maps[m].start + maps[m].off);
                    shown++;
                    break;
                }
            }
        }
        fflush(stderr);
    }
    _exit(128 + sig);
}

static void term_handler(int sig) {
    _exit(0);
}

typedef struct video_renderer_webos_s {
    video_renderer_t base;
    GstElement *appsrc, *audio_appsrc, *rtp_sink, *pipeline;
    GstClock *clock;
    pthread_mutex_t lock;
    int udp_fd;                /* RTP to the TV pipeline */
    long long send_due_us;     /* pacing: when the bytes sent so far are due */

    int connections;
    int app_launched;
    int tv_ready;              /* TV pipeline bound to UDP_PORT and settled */
    long long launch_ms;       /* when the app was last (re)launched */
    long long bound_ms;        /* when the port was first seen bound, 0 if not yet */
    unsigned long stale_inode; /* socket of the pipeline being replaced, 0 once closed */
    int restart_pending;       /* size changed while a pipeline was starting */
    long long primed_ms;       /* when the GOP was replayed to the current pipeline, 0 if not */
    unsigned primed_drops;     /* the TV socket's drop count at that replay */
    int replays;               /* extra replays to the current pipeline */
    int tv_showing;            /* the app reported umediaserver "playing" */
    int need_idr;              /* nothing decodable cached; wait for the next IDR */
    int gop_valid;             /* gop[0] is an IDR */

    /* Debug: with RPIPLAY_DUMP_DIR set, each prime and the frames after it
     * are written to <dir>/prime-N.h264 */
    FILE *dump;
    int dump_frames_left, dump_count;
    long long last_ready_check;

    /* Stats, logged every STATS_SECONDS while streaming */
    time_t last_stats;
    unsigned video_pushed, audio_pushed;

    int width, height;         /* size the TV player is launched with */
    int stream_width, stream_height; /* current H.264 picture size */
    GstBuffer *sps_pps;        /* last codec config (frame_type 0) */
    GstBuffer *gop[GOP_MAX_BUFFERS];
    int gop_count;
    size_t gop_bytes;
} video_renderer_webos_t;

static const video_renderer_funcs_t video_renderer_webos_funcs;

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static long long now_ms(void) {
    return now_us() / 1000;
}

static void luna_send(const char *uri, const char *payload) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "luna-send -n 1 -w 5000 %s '%s' 2>&1", uri, payload);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("[webos-video] popen failed for %s\n", uri);
        return;
    }
    char result[256];
    if (fgets(result, sizeof(result), fp))
        printf("[webos-video] %s -> %s", uri, result);
    else
        printf("[webos-video] %s -> (no reply; running inside the dev-mode jail?)\n", uri);
    pclose(fp);
    fflush(stdout);
}

static void launch_app(video_renderer_webos_t *r) {
    char payload[256];
    if (!r->width) {
        r->width = 1920;
        r->height = 1080;
    }
    snprintf(payload, sizeof(payload),
             "{\"id\":\"" APP_ID "\",\"params\":{\"udpPort\":%d,\"width\":%d,\"height\":%d,\"framerate\":30,"
             "\"audioCodec\":\"AAC\",\"audioRate\":%d,\"audioChannels\":%d}}",
             UDP_PORT, r->width, r->height, WEBOS_AUDIO_RATE, WEBOS_AUDIO_CHANNELS);
    luna_send("luna://com.webos.applicationManager/launch", payload);
    r->app_launched = 1;
    r->tv_ready = 0;
    r->restart_pending = 0;
    r->primed_ms = 0;
    r->replays = 0;
    r->need_idr = 0;
    r->launch_ms = now_ms();
    r->bound_ms = 0;
    r->stale_inode = 0;
    r->last_ready_check = 0;
}

static int udp_port_bound(void);
static unsigned long udp_port_inode(void);

/* umediaserver runs each media pipeline as its own process */
static int media_pipeline_running(void) {
    DIR *proc = opendir("/proc");
    struct dirent *e;
    int found = 0;
    if (!proc) return 0;
    while (!found && (e = readdir(proc)) != NULL) {
        char path[64], cmd[64];
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        cmd[n] = 0;
        if (strstr(cmd, "starfish-media-pipeline")) found = 1;
    }
    closedir(proc);
    return found;
}

static void close_app(video_renderer_webos_t *r) {
    luna_send("luna://com.webos.applicationManager/closeByAppId", "{\"id\":\"" APP_ID "\"}");
    r->app_launched = 0;
    r->tv_ready = 0;
    r->bound_ms = 0;
    /* Wait for the old pipeline process to exit, releasing the port and the
     * TV's video decoder. A new pipeline started while the old one still
     * held the decoder stayed black. */
    long long start = now_ms();
    while ((udp_port_bound() || media_pipeline_running()) &&
           now_ms() - start < PIPELINE_EXIT_TIMEOUT_MS)
        usleep(50000);
    printf("[webos-video] old pipeline gone after %lld ms%s\n", now_ms() - start,
           media_pipeline_running() ? " (timed out, still running)" : "");
    fflush(stdout);
}

/* The Miracast pipeline binds 0.0.0.0:UDP_PORT once the app's <video> loads;
 * the socket's inode identifies the pipeline instance. */
typedef struct {
    unsigned long inode;       /* 0 if none */
    unsigned rx_queue;         /* receive buffer in use, bytes */
    unsigned drops;            /* datagrams dropped because it was full */
} udp_socket_t;

/* Fills in a socket on UDP_PORT other than 'stale' and returns whether
 * 'stale' is still bound too. */
static int udp_port_scan(unsigned long stale, udp_socket_t *s) {
    char line[256];
    int stale_bound = 0;
    memset(s, 0, sizeof(*s));
    FILE *f = fopen("/proc/net/udp", "r");
    if (!f) return 0;
    fgets(line, sizeof(line), f);   /* header */
    while (fgets(line, sizeof(line), f)) {
        /* sl local:port rem:port st tx_queue:rx_queue tr:when retrnsmt uid timeout inode ref pointer drops */
        unsigned int port, rx_queue, drops = 0;
        unsigned long inode;
        if (sscanf(line, " %*d: %*x:%x %*x:%*x %*x %*x:%x %*x:%*x %*x %*u %*d %lu %*d %*s %u",
                   &port, &rx_queue, &inode, &drops) < 3 || port != UDP_PORT)
            continue;
        if (inode == stale) {
            stale_bound = 1;
        } else {
            s->inode = inode;
            s->rx_queue = rx_queue;
            s->drops = drops;
        }
    }
    fclose(f);
    return stale_bound;
}

static unsigned long udp_port_inode(void) {
    udp_socket_t s;
    udp_port_scan(0, &s);
    return s.inode;
}

static int udp_port_bound(void) {
    return udp_port_inode() != 0;
}

/* Call with the lock held. */
static void check_ready(video_renderer_webos_t *r, long long now) {
    if (r->tv_ready) return;
    udp_socket_t s;
    int stale_bound = udp_port_scan(r->stale_inode, &s);
    if (r->stale_inode && !stale_bound) {
        printf("[webos-video] old pipeline's socket closed (%lld ms after launch)\n", now - r->launch_ms);
        fflush(stdout);
        r->stale_inode = 0;
    }
    if (!r->bound_ms && s.inode) {
        printf("[webos-video] TV pipeline listening on %d (%lld ms after launch)\n",
               UDP_PORT, now - r->launch_ms);
        fflush(stdout);
        r->bound_ms = now;
    }
    /* Mid-stream there's a GOP to replay; at connection setup iOS waits */
    long long settle = r->gop_count ? REPLAY_SETTLE_MS : TV_SETTLE_MS;
    if (r->bound_ms && now - r->bound_ms >= settle) {
        /* While both are bound the kernel may deliver to either, and a
         * replay sent to the dying pipeline is lost */
        if (stale_bound && now - r->bound_ms < settle + STALE_SOCKET_WAIT_MS)
            return;
        printf("[webos-video] TV pipeline ready (%lld ms after launch)%s\n", now - r->launch_ms,
               stale_bound ? ", but the old pipeline's socket is still open" : "");
        fflush(stdout);
        r->tv_ready = 1;
    }
}

/* The TV's Miracast decoder can't follow a resolution change (rotation), so
 * relaunch the app: its webOSRelaunch handler replaces the <video>, giving a
 * fresh pipeline. Frames are cached until that one is ready, then primed.
 * Closing the app instead was no more reliable, slower, and brought Live TV
 * to the front (restarting its tuner pipeline) on every rotation.
 * Call with the lock held. */
static void restart_tv_pipeline(video_renderer_webos_t *r) {
    printf("[webos-video] restarting TV pipeline\n");
    fflush(stdout);
    /* The old pipeline keeps the port for ~0.4 s after the relaunch; don't
     * mistake it for the new one. */
    unsigned long old_inode = udp_port_inode();
    launch_app(r);
    r->stale_inode = old_inode;
}

static void gop_clear(video_renderer_webos_t *r) {
    for (int i = 0; i < r->gop_count; i++)
        gst_buffer_unref(r->gop[i]);
    r->gop_count = 0;
    r->gop_bytes = 0;
}

/* Buffers are stamped with the pipeline running time when they arrive, not
 * when appsrc's thread dequeues them (do-timestamp). mpegtsmux interleaves by
 * timestamp and needs a buffer on both pads; with dequeue-time stamps every
 * queued video frame looked newer than the waiting audio frame, so only a
 * frame or two got through per audio frame and video fell seconds behind at
 * 60 fps. Arrival stamps also give the TV the real capture timing. */
static void stamp(video_renderer_webos_t *r, GstBuffer *buffer) {
    if (!r->clock) return;
    GstClockTime now = gst_clock_get_time(r->clock) - gst_element_get_base_time(r->pipeline);
    GST_BUFFER_PTS(buffer) = now;
    GST_BUFFER_DTS(buffer) = now;
}

static void dump_buffer(video_renderer_webos_t *r, GstBuffer *buffer) {
    GstMapInfo map;
    if (!r->dump) return;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        fwrite(map.data, 1, map.size, r->dump);
        gst_buffer_unmap(buffer, &map);
    }
    if (--r->dump_frames_left <= 0) {
        fclose(r->dump);
        r->dump = NULL;
    }
}

static void push(video_renderer_webos_t *r, GstBuffer *buffer) {
    dump_buffer(r, buffer);
    stamp(r, buffer);
    gst_app_src_push_buffer(GST_APP_SRC(r->appsrc), buffer);
    r->video_pushed++;
}

/* Call with the lock held. Shows whether data backs up on our side. */
static void log_stats(video_renderer_webos_t *r, time_t now) {
    if (now - r->last_stats < STATS_SECONDS) return;
    guint64 vq = 0, aq = 0;
    g_object_get(r->appsrc, "current-level-bytes", &vq, NULL);
    g_object_get(r->audio_appsrc, "current-level-bytes", &aq, NULL);
    udp_socket_t s;
    udp_port_scan(r->stale_inode, &s);
    printf("[webos-stats] %lds: video %u frames, queued %llu B | audio %u frames, queued %llu B"
           " | TV socket %u B, %u drops\n",
           (long)(now - r->last_stats), r->video_pushed, (unsigned long long)vq,
           r->audio_pushed, (unsigned long long)aq, s.rx_queue, s.drops);
    fflush(stdout);
    r->video_pushed = r->audio_pushed = 0;
    r->last_stats = now;
}

/* Replay codec config + GOP to the TV pipeline. It must be followed by more
 * frames: the TV's demuxer holds each frame until the next one starts, and
 * an IDR sent on its own was never shown. (libpf-miracastplugin also flushes
 * its UDP socket on play, but only when resuming from pause, not on a new
 * pipeline's first play.) The cache keeps its own buffers; pushes get
 * copies. Call with the lock held. */
static void prime(video_renderer_webos_t *r) {
    r->tv_showing = 0;
    if (!r->gop_valid || r->gop_count == 0) {
        printf("[webos-video] no keyframe cached, waiting for the next one\n");
        fflush(stdout);
        r->need_idr = 1;
        r->primed_ms = 0;
        return;
    }
    udp_socket_t s;
    udp_port_scan(r->stale_inode, &s);
    r->primed_ms = now_ms();
    r->primed_drops = s.drops;
    printf("[webos-video] priming TV decoder: sps/pps=%s, %d frames (%u bytes); "
           "TV socket %u bytes queued, %u drops so far\n",
           r->sps_pps ? "yes" : "no", r->gop_count, (unsigned)r->gop_bytes, s.rx_queue, s.drops);
    fflush(stdout);
    const char *dir = getenv("RPIPLAY_DUMP_DIR");
    if (dir) {
        char path[256];
        if (r->dump) fclose(r->dump);
        mkdir(dir, 0755);
        snprintf(path, sizeof(path), "%s/prime-%d.h264", dir, ++r->dump_count);
        r->dump = fopen(path, "wb");
        r->dump_frames_left = 1 + r->gop_count + 180;
    }
    if (r->sps_pps) push(r, gst_buffer_copy(r->sps_pps));
    for (int i = 0; i < r->gop_count; i++)
        push(r, gst_buffer_copy(r->gop[i]));
}

/* appsink callback: sends one RTP packet to the TV pipeline, paced to
 * SEND_RATE (see there). Runs on our streaming thread, so waiting here only
 * lets our appsrc queues absorb the backlog. */
static GstFlowReturn send_rtp(GstAppSink *sink, gpointer user_data) {
    video_renderer_webos_t *r = user_data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_EOS;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        long long now = now_us();
        if (r->send_due_us < now) r->send_due_us = now;
        long long ahead = r->send_due_us - now - SEND_BURST * 1000000LL / SEND_RATE;
        if (ahead > 0) usleep(ahead);
        r->send_due_us += map.size * 1000000LL / SEND_RATE;
        send(r->udp_fd, map.data, map.size, 0);   /* fails while no pipeline listens */
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Minimal H.264 SPS reader: just enough to get the coded picture size. */
typedef struct {
    unsigned char rbsp[256];
    int len, pos;   /* pos in bits */
} bits_t;

static unsigned bits_u(bits_t *b, int n) {
    unsigned v = 0;
    while (n--) {
        int byte = b->pos >> 3;
        v = (v << 1) | (byte < b->len ? (b->rbsp[byte] >> (7 - (b->pos & 7))) & 1 : 0);
        b->pos++;
    }
    return v;
}

static unsigned bits_ue(bits_t *b) {
    int zeros = 0;
    while (bits_u(b, 1) == 0 && zeros < 32) zeros++;
    return ((1u << zeros) - 1) + bits_u(b, zeros);
}

static int bits_se(bits_t *b) {
    unsigned v = bits_ue(b);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static int sps_size(const unsigned char *data, int data_len, int *width, int *height) {
    bits_t b = { .len = 0, .pos = 0 };
    int i;
    /* Find the SPS NAL (type 7) after a start code */
    for (i = 0; i + 3 < data_len; i++)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 7)
            break;
    if (i + 3 >= data_len) return 0;
    /* Unescape emulation prevention bytes up to the next start code */
    for (i += 4; i < data_len && b.len < (int)sizeof(b.rbsp); i++) {
        if (i + 2 < data_len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] <= 1) break;
        if (i + 2 < data_len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
            b.rbsp[b.len++] = 0;
            b.rbsp[b.len++] = 0;
            i += 2;
            continue;
        }
        b.rbsp[b.len++] = data[i];
    }

    unsigned profile = bits_u(&b, 8);
    bits_u(&b, 16);                         /* constraint flags, level */
    bits_ue(&b);                            /* sps id */
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = bits_ue(&b);
        if (chroma == 3) bits_u(&b, 1);
        bits_ue(&b);                        /* bit depth luma */
        bits_ue(&b);                        /* bit depth chroma */
        bits_u(&b, 1);
        if (bits_u(&b, 1)) {                /* scaling matrices */
            for (int n = 0; n < (chroma == 3 ? 12 : 8); n++) {
                if (!bits_u(&b, 1)) continue;
                int size = n < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < size && next; j++) {
                    next = (last + bits_se(&b) + 256) % 256;
                    if (next) last = next;
                }
            }
        }
    }
    bits_ue(&b);                            /* log2 max frame num */
    unsigned poc_type = bits_ue(&b);
    if (poc_type == 0) {
        bits_ue(&b);
    } else if (poc_type == 1) {
        bits_u(&b, 1);
        bits_se(&b);
        bits_se(&b);
        unsigned n = bits_ue(&b);
        for (unsigned j = 0; j < n && j < 256; j++) bits_se(&b);
    }
    bits_ue(&b);                            /* max ref frames */
    bits_u(&b, 1);
    unsigned w_mbs = bits_ue(&b) + 1;
    unsigned h_units = bits_ue(&b) + 1;
    unsigned frame_mbs_only = bits_u(&b, 1);
    if (!frame_mbs_only) bits_u(&b, 1);
    bits_u(&b, 1);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (bits_u(&b, 1)) {
        cl = bits_ue(&b); cr = bits_ue(&b); ct = bits_ue(&b); cb = bits_ue(&b);
    }
    unsigned sub_w = chroma == 1 || chroma == 2 ? 2 : 1;
    unsigned sub_h = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
    *width = w_mbs * 16 - (cl + cr) * sub_w;
    *height = (2 - frame_mbs_only) * h_units * 16 - (ct + cb) * sub_h;
    return *width > 0 && *height > 0;
}

static int is_idr(unsigned char *data, int data_len) {
    /* Annex B access unit: look for a NAL of type 5 */
    for (int i = 0; i + 4 < data_len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 5)
            return 1;
    }
    return 0;
}

/* Startup logs "gst_structure_get_boolean: assertion `structure != NULL'
 * failed" twice: LG's appsrc reads a "directv-rvu" flag from basesrc's
 * LG-added "smart-properties" structure, which is unset. Harmless. Setting an
 * empty structure silences it, but made the TV show black after every
 * rotation, so it is left unset. */

/* The app reports <video> and umediaserver events as HTTP GETs of
 * http://127.0.0.1:APP_EVENT_PORT/<url-encoded message>. Log them with the
 * time since the last (re)launch. */
static void url_decode(char *s) {
    char *out = s;
    for (; *s; s++) {
        unsigned int c;
        if (*s == '%' && sscanf(s + 1, "%2x", &c) == 1) {
            *out++ = (char)c;
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = 0;
}

static void *app_event_thread(void *arg) {
    video_renderer_webos_t *r = arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(APP_EVENT_PORT) };
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 8) < 0) {
        printf("[webos-app] can't listen on %d for app events\n", APP_EVENT_PORT);
        fflush(stdout);
        return NULL;
    }
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        char req[2048];
        int n = recv(c, req, sizeof(req) - 1, 0);
        static const char reply[] = "HTTP/1.0 204 No Content\r\nConnection: close\r\n\r\n";
        send(c, reply, sizeof(reply) - 1, 0);
        close(c);
        if (n <= 0) continue;
        req[n] = 0;
        /* "GET /<message>?<seq> HTTP/1.1" */
        char *msg = strchr(req, '/');
        if (!msg || strncmp(req, "GET ", 4) != 0) continue;
        msg++;
        msg[strcspn(msg, "? \r\n")] = 0;
        url_decode(msg);
        pthread_mutex_lock(&r->lock);
        long long now = now_ms(), since = now - r->launch_ms, after_prime = -1;
        unsigned long stale = r->stale_inode;
        unsigned primed_drops = r->primed_drops;
        if (strstr(msg, "playbackNotificationInfo") && strstr(msg, "\"playing\"")) {
            if (r->primed_ms && !r->tv_showing) after_prime = now - r->primed_ms;
            r->tv_showing = 1;
        }
        pthread_mutex_unlock(&r->lock);
        printf("[webos-app] +%lld ms: %s\n", since, msg);
        if (after_prime >= 0) {
            udp_socket_t s;
            udp_port_scan(stale, &s);
            printf("[webos-video] picture %lld ms after replay, %u drops since\n",
                   after_prime, s.drops - primed_drops);
        }
        fflush(stdout);
    }
    return NULL;
}

video_renderer_t *video_renderer_gstreamer_init(logger_t *logger, video_renderer_config_t const *config) {
    video_renderer_webos_t *renderer;
    GError *error = NULL;

    renderer = calloc(1, sizeof(video_renderer_webos_t));
    assert(renderer);

    {
        struct sigaction sa;
        sa.sa_sigaction = crash_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGTRAP, &sa, NULL);  /* G_DEBUG=fatal-criticals */
    }
    signal(SIGTERM, term_handler);
    signal(SIGINT, term_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, usr1_handler);

    gst_init(NULL, NULL);

    renderer->base.logger = logger;
    renderer->base.funcs = &video_renderer_webos_funcs;
    renderer->base.type = VIDEO_RENDERER_GSTREAMER;
    pthread_mutex_init(&renderer->lock, NULL);

    char launch[768];
    snprintf(launch, sizeof(launch),
             "appsrc name=video_source is-live=true format=time stream-type=0 ! "
             "h264parse config-interval=1 ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! mux. "
             "appsrc name=audio_source is-live=true format=time stream-type=0 ! "
             "aacparse ! mux. "
             "mpegtsmux name=mux ! rtpmp2tpay ! "
             "appsink name=rtp_sink sync=false async=false");
    printf("[webos-video] pipeline: %s\n", launch);
    renderer->pipeline = gst_parse_launch(launch, &error);
    if (error) {
        printf("[webos-video] pipeline failed: %s\n", error->message);
        g_error_free(error);
        if (renderer->pipeline) gst_object_unref(renderer->pipeline);
        free(renderer);
        return NULL;
    }

    renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_source");
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        NULL);
    g_object_set(renderer->appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    renderer->audio_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_source");
    caps = gst_caps_new_simple("audio/mpeg",
        "mpegversion", G_TYPE_INT, 4,
        "stream-format", G_TYPE_STRING, "adts",
        "rate", G_TYPE_INT, WEBOS_AUDIO_RATE,
        "channels", G_TYPE_INT, WEBOS_AUDIO_CHANNELS,
        NULL);
    g_object_set(renderer->audio_appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    renderer->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in tv = { .sin_family = AF_INET, .sin_port = htons(UDP_PORT) };
    tv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (renderer->udp_fd < 0 || connect(renderer->udp_fd, (struct sockaddr *)&tv, sizeof(tv)) < 0)
        printf("[webos-video] can't create the RTP socket\n");
    renderer->rtp_sink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "rtp_sink");
    GstAppSinkCallbacks callbacks = { .new_sample = send_rtp };
    gst_app_sink_set_callbacks(GST_APP_SINK(renderer->rtp_sink), &callbacks, renderer, NULL);

    pthread_t events;
    if (pthread_create(&events, NULL, app_event_thread, renderer) == 0)
        pthread_detach(events);

    printf("[webos-video] init complete\n");
    fflush(stdout);
    return &renderer->base;
}

static void video_renderer_webos_start(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    GstStateChangeReturn ret = gst_element_set_state(r->pipeline, GST_STATE_PLAYING);
    gst_element_get_state(r->pipeline, NULL, NULL, GST_SECOND);
    r->clock = gst_element_get_clock(r->pipeline);
    printf("[webos-video] PLAYING returned %d, clock %p\n", ret, (void*)r->clock);
    fflush(stdout);
}

static void video_renderer_webos_render_buffer(video_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts, int type) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;

    if (data_len == 0) return;

    pthread_mutex_lock(&r->lock);

    /* Covers a stream that starts without a conn_init (or after the app was closed) */
    if (!r->app_launched)
        launch_app(r);

    /* Keep codec config and the current GOP */
    GstBuffer *cached = gst_buffer_new_and_alloc(data_len);
    assert(cached != NULL);
    gst_buffer_fill(cached, 0, data, data_len);
    if (type == 0) {
        if (r->sps_pps) gst_buffer_unref(r->sps_pps);
        r->sps_pps = cached;
        /* The TV player can't follow a picture size change (rotation), so
         * give it a fresh pipeline. It is always launched as 1920x1080. */
        int w, h;
        if (sps_size(data, data_len, &w, &h)) {
            if (w != r->stream_width || h != r->stream_height) {
                printf("[webos-video] stream size %dx%d -> %dx%d\n",
                       r->stream_width, r->stream_height, w, h);
                fflush(stdout);
                int was_streaming = r->stream_width != 0;
                r->stream_width = w;
                r->stream_height = h;
                /* One restart at a time: overlapping ones left the TV's
                 * decoder stuck on black */
                if (was_streaming && r->app_launched) {
                    if (r->tv_ready)
                        restart_tv_pipeline(r);
                    else
                        r->restart_pending = 1;
                }
            }
        }
    } else {
        if (is_idr(data, data_len)) {
            gop_clear(r);
            r->gop_valid = 1;
        } else if (r->gop_count == GOP_MAX_BUFFERS || r->gop_bytes + data_len > GOP_MAX_BYTES) {
            /* Frames after this are useless without the IDR being dropped,
             * so cache nothing until the next one */
            printf("[webos-video] GOP cache full, dropping it until the next keyframe\n");
            fflush(stdout);
            gop_clear(r);
            r->gop_valid = 0;
        }
        if (r->gop_valid) {
            r->gop[r->gop_count++] = gst_buffer_ref(cached);
            r->gop_bytes += data_len;
        }
    }

    if (!r->tv_ready) {
        long long now = now_ms();
        if (now - r->last_ready_check >= TV_READY_POLL_MS) {
            r->last_ready_check = now;
            check_ready(r, now);
        }
        /* Became ready mid-stream: the current frame is already in the GOP.
         * If the picture size changed while it started, restart again. */
        if (r->tv_ready && r->restart_pending)
            restart_tv_pipeline(r);
        else if (r->tv_ready)
            prime(r);
    } else if (prime_requested) {
        prime_requested = 0;
        prime(r);
    } else if (r->primed_ms && !r->tv_showing &&
               now_ms() - r->primed_ms > (r->replays ? PICTURE_TIMEOUT_MS : REPLAY_RETRY_MS)) {
        udp_socket_t s;
        udp_port_scan(r->stale_inode, &s);
        int replay = r->replays < REPLAYS_BEFORE_RESTART;
        printf("[webos-video] no picture %lld ms after replay (TV socket %u bytes queued, %u drops since), %s\n",
               now_ms() - r->primed_ms, s.rx_queue, s.drops - r->primed_drops,
               replay ? "replaying again" : "restarting TV pipeline");
        fflush(stdout);
        if (replay) {
            r->replays++;
            prime(r);
        } else {
            restart_tv_pipeline(r);
        }
    } else if (r->need_idr) {
        /* The current frame is cached only if it was an IDR */
        if (r->gop_valid && r->gop_count == 1) {
            r->need_idr = 0;
            prime(r);
        }
    } else {
        push(r, gst_buffer_copy(cached));
    }
    if (r->tv_ready)
        log_stats(r, time(NULL));
    if (type != 0)
        gst_buffer_unref(cached);   /* the GOP holds its own reference */
    pthread_mutex_unlock(&r->lock);
}

/* Audio only flows while video is being sent: mpegtsmux waits for both
 * inputs, so audio queued outside a session would pile up in appsrc. */
void video_renderer_webos_push_audio(video_renderer_t *renderer, const unsigned char *adts, int len) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    pthread_mutex_lock(&r->lock);
    int streaming = r->tv_ready;
    if (streaming) r->audio_pushed++;
    pthread_mutex_unlock(&r->lock);
    if (!streaming) return;

    GstBuffer *buffer = gst_buffer_new_and_alloc(len);
    gst_buffer_fill(buffer, 0, adts, len);
    stamp(r, buffer);
    gst_app_src_push_buffer(GST_APP_SRC(r->audio_appsrc), buffer);
}

void video_renderer_webos_flush(video_renderer_t *renderer) {
}

void video_renderer_webos_destroy(video_renderer_t *renderer) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    gst_element_set_state(r->pipeline, GST_STATE_NULL);
    if (r->clock) gst_object_unref(r->clock);
    gst_object_unref(r->rtp_sink);
    gst_object_unref(r->pipeline);
    if (r->udp_fd >= 0) close(r->udp_fd);
    if (r->app_launched) close_app(r);
    gop_clear(r);
    if (r->sps_pps) gst_buffer_unref(r->sps_pps);
    pthread_mutex_destroy(&r->lock);
    free(renderer);
}

/* Called with +1 on connection init and -1 on connection teardown.
 * The app is shown while at least one client connection is open. */
void video_renderer_webos_update_background(video_renderer_t *renderer, int type) {
    video_renderer_webos_t *r = (video_renderer_webos_t *)renderer;
    pthread_mutex_lock(&r->lock);
    if (type > 0) {
        r->connections++;
        if (!r->app_launched) {
            launch_app(r);
            pthread_mutex_unlock(&r->lock);

            /* Hold this connection until the TV can decode the first IDR */
            int ready = 0;
            for (int i = 0; i < TV_READY_TIMEOUT_SECONDS * 10 && !ready; i++) {
                usleep(100000);
                pthread_mutex_lock(&r->lock);
                check_ready(r, now_ms());
                ready = r->tv_ready;
                pthread_mutex_unlock(&r->lock);
            }
            if (!ready) {
                printf("[webos-video] TV pipeline not ready after %ds, continuing\n",
                       TV_READY_TIMEOUT_SECONDS);
                fflush(stdout);
            }
            return;
        }
    } else if (type < 0 && r->connections > 0) {
        r->connections--;
        if (r->connections == 0) {
            if (r->app_launched) close_app(r);
            gop_clear(r);
            audio_renderer_webos_session_end();
            r->stream_width = r->stream_height = 0;
            if (r->sps_pps) {
                gst_buffer_unref(r->sps_pps);
                r->sps_pps = NULL;
            }
        }
    }
    pthread_mutex_unlock(&r->lock);
}

static const video_renderer_funcs_t video_renderer_webos_funcs = {
    .start = video_renderer_webos_start,
    .render_buffer = video_renderer_webos_render_buffer,
    .flush = video_renderer_webos_flush,
    .destroy = video_renderer_webos_destroy,
    .update_background = video_renderer_webos_update_background,
};
