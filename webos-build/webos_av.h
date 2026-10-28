/*
 * Shared between the webOS video and audio renderers: audio is muxed into the
 * video renderer's MPEG-TS stream so the TV's Miracast pipeline plays both.
 */
#ifndef WEBOS_AV_H
#define WEBOS_AV_H

#include "video_renderer.h"

#define WEBOS_AUDIO_RATE 44100
#define WEBOS_AUDIO_CHANNELS 2

/* Push one ADTS AAC-LC frame. Dropped unless a mirroring session is streaming. */
void video_renderer_webos_push_audio(video_renderer_t *renderer, const unsigned char *adts, int len);

#endif
