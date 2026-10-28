/*
 * Audio renderer for LG webOS TVs using GStreamer with pulsesink
 * Based on RPiPlay's audio_renderer_gstreamer.c
 */

#include "audio_renderer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/app/gstappsrc.h>

typedef struct audio_renderer_webos_s {
    audio_renderer_t base;
    GstElement *appsrc;
    GstElement *pipeline;
    GstElement *volume;
} audio_renderer_webos_t;

static const audio_renderer_funcs_t audio_renderer_webos_funcs;

audio_renderer_t *audio_renderer_gstreamer_init(logger_t *logger, video_renderer_t *video_renderer, audio_renderer_config_t const *config) {
    audio_renderer_webos_t *renderer;
    GError *error = NULL;

    renderer = calloc(1, sizeof(audio_renderer_webos_t));
    if (!renderer) {
        return NULL;
    }
    renderer->base.logger = logger;
    renderer->base.funcs = &audio_renderer_webos_funcs;
    renderer->base.type = AUDIO_RENDERER_GSTREAMER;

    printf("[webos-audio] init start\n");
    fflush(stdout);

    if (!video_renderer || video_renderer->type != VIDEO_RENDERER_GSTREAMER) {
        gst_init(NULL, NULL);
    }

    const char *pipelines[] = {
        "appsrc name=audio_source stream-type=0 ! "
        "aacparse ! omxaacdec ! audioconvert ! pulsesink name=volume",

        "appsrc name=audio_source stream-type=0 ! "
        "decodebin ! audioconvert ! pulsesink name=volume",

        "appsrc name=audio_source ! "
        "fakesink name=volume sync=false",
        NULL
    };

    for (int i = 0; pipelines[i] != NULL; i++) {
        error = NULL;
        printf("[webos-audio] trying pipeline %d: %s\n", i, pipelines[i]);
        fflush(stdout);
        renderer->pipeline = gst_parse_launch(pipelines[i], &error);
        if (renderer->pipeline && !error) {
            printf("[webos-audio] Using pipeline %d\n", i);
            fflush(stdout);
            break;
        }
        if (error) {
            printf("[webos-audio] Pipeline %d failed: %s\n", i, error->message);
            fflush(stdout);
            g_error_free(error);
        }
        if (renderer->pipeline) {
            gst_object_unref(renderer->pipeline);
            renderer->pipeline = NULL;
        }
    }

    if (!renderer->pipeline) {
        printf("[webos-audio] No working pipeline found!\n");
        free(renderer);
        return NULL;
    }

    renderer->appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_source");
    renderer->volume = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "volume");

    /* Set AAC-ELD caps: 44100Hz stereo with codec_data */
    unsigned char eld_conf[] = {0xF8, 0xE8, 0x50, 0x00};
    GstBuffer *codec_data = gst_buffer_new_and_alloc(sizeof(eld_conf));
    if (codec_data) {
        gst_buffer_fill(codec_data, 0, eld_conf, sizeof(eld_conf));
        GstCaps *caps = gst_caps_new_simple("audio/mpeg",
            "rate", G_TYPE_INT, 44100,
            "channels", G_TYPE_INT, 2,
            "mpegversion", G_TYPE_INT, 4,
            "stream-format", G_TYPE_STRING, "raw",
            /* Not GST_TYPE_BUFFER: referencing the _gst_buffer_type variable
             * makes the stub-linked binary copy-relocate it, and plugins then
             * see a stale 0 GType (crashes mpegtsmux) */
            "codec_data", gst_buffer_get_type(), codec_data,
            NULL);
        if (caps) {
            printf("[webos-audio] setting AAC-ELD caps\n");
            fflush(stdout);
            g_object_set(renderer->appsrc, "caps", caps, NULL);
            gst_caps_unref(caps);
            printf("[webos-audio] caps set OK\n");
            fflush(stdout);
        }
        gst_buffer_unref(codec_data);
    }

    printf("[webos-audio] init complete, appsrc=%p volume=%p\n",
           (void*)renderer->appsrc, (void*)renderer->volume);
    fflush(stdout);
    return &renderer->base;
}

void audio_renderer_webos_start(audio_renderer_t *renderer) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    GstStateChangeReturn ret = gst_element_set_state(r->pipeline, GST_STATE_PLAYING);
    printf("[webos-audio] PLAYING returned %d\n", ret);
    fflush(stdout);
}

void audio_renderer_webos_render_buffer(audio_renderer_t *renderer, raop_ntp_t *ntp, unsigned char *data, int data_len, uint64_t pts) {
    if (data_len == 0) return;
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;

    GstBuffer *buffer = gst_buffer_new_and_alloc(data_len);
    assert(buffer != NULL);
    GST_BUFFER_DTS(buffer) = (GstClockTime)pts;
    gst_buffer_fill(buffer, 0, data, data_len);
    gst_app_src_push_buffer(GST_APP_SRC(r->appsrc), buffer);
}

void audio_renderer_webos_set_volume(audio_renderer_t *renderer, float volume) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    if (r->volume && fabs(volume) < 28) {
        float avol = floorf(((28-fabs(volume))/28)*10)/10;
        g_object_set(r->volume, "volume", avol, NULL);
    }
}

void audio_renderer_webos_flush(audio_renderer_t *renderer) {
}

void audio_renderer_webos_destroy(audio_renderer_t *renderer) {
    audio_renderer_webos_t *r = (audio_renderer_webos_t *)renderer;
    gst_element_set_state(r->pipeline, GST_STATE_NULL);
    gst_object_unref(r->pipeline);
    free(renderer);
}

static const audio_renderer_funcs_t audio_renderer_webos_funcs = {
    .start = audio_renderer_webos_start,
    .render_buffer = audio_renderer_webos_render_buffer,
    .set_volume = audio_renderer_webos_set_volume,
    .flush = audio_renderer_webos_flush,
    .destroy = audio_renderer_webos_destroy,
};
