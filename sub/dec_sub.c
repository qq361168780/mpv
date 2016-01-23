/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>

#include "config.h"
#include "demux/demux.h"
#include "sd.h"
#include "dec_sub.h"
#include "options/options.h"
#include "common/global.h"
#include "common/msg.h"
#include "osdep/threads.h"
#include "video/mp_image.h"

extern const struct sd_functions sd_ass;
extern const struct sd_functions sd_lavc;

static const struct sd_functions *const sd_list[] = {
    &sd_lavc,
#if HAVE_LIBASS
    &sd_ass,
#endif
    NULL
};

// It's hard to put an upper bound on the ahead rendering caused by use of
// vo_opengl interpolation + blend-subtitles.
#define MAX_BUFFER (MAX_SUB_RENDER_AHEAD + 10)

struct dec_sub {
    // --- the following fields are invariant after init
    struct mp_log *log;
    struct MPOpts *opts;

    struct sh_stream *sh;

    bool threaded;
    pthread_t thread;

    // --- the following fields are protected by state_lock
    pthread_mutex_t state_lock;
    pthread_cond_t state_wakeup;
    double last_pkt_pts;
    struct mp_image_params last_video_fmt;
    struct mp_osd_res last_osd_res;
    struct cache_entry *entries[MAX_BUFFER];
    int num_entries;
    struct demux_packet **packets;
    int num_packets;
    bool preloaded;
    struct cache_entry *cur;

    // This lock serialized accesses to sub_get_bitmaps(). The problem is that
    // a call to sub_get_bitmaps() can invalidate the result to the previous
    // call, so sub_get_bitmaps() "reserves" access to the renderer, and must
    // wait until the previous reservation has expired.
    bool reserved;

    // --- the following fields are protected by sd_lock
    // (lock order: lock state_lock before sd_lock)
    pthread_mutex_t sd_lock;
    struct sd *sd;
};

struct cache_entry {
    // all fields, including refcount, are protected by state_lock, or immutable
    double pts;
    int refcount;
    bool rendered;
    struct sub_bitmaps data;
};

static void copy_sub_bitmaps(struct sub_bitmaps *dst, struct sub_bitmaps *src)
{
    assert(src->format == SUBBITMAP_EMPTY || src->format == SUBBITMAP_LIBASS);
    *dst = *src;
    dst->parts =
        talloc_memdup(NULL, src->parts, sizeof(dst->parts[0]) * src->num_parts);
    for (int n = 0; n < dst->num_parts; n++) {
        struct sub_bitmap *p = &dst->parts[n];
        p->bitmap = talloc_memdup(dst->parts, p->bitmap, p->stride * p->h);
    }
}

static void cache_entry_unref(struct cache_entry *e)
{
    if (e) {
        e->refcount -= 1;
        if (e->refcount == 0) {
            talloc_free(e->data.parts);
            talloc_free(e);
        }
    }
}

static void *sub_thread(void *arg)
{
    struct dec_sub *sub = arg;
    mpthread_set_name("subrender");

    pthread_mutex_lock(&sub->state_lock);
    while (sub->threaded) {
        for (int n = 0; n < sub->num_packets; n++) {
            sub->sd->driver->decode(sub->sd, sub->packets[n]);
            talloc_free(sub->packets[n]);
        }
        sub->num_packets = 0;

        struct cache_entry *e = NULL;
        for (int n = 0; n < sub->num_entries; n++) {
            if (!sub->entries[n]->rendered) {
                e = sub->entries[n];
                break;
            }
        }

        struct mp_osd_res res = sub->last_osd_res;
        if (e && res.w && res.h) {
            assert(e->refcount);
            e->refcount += 1;
            pthread_mutex_unlock(&sub->state_lock);
            pthread_mutex_lock(&sub->sd_lock);

            struct sub_bitmaps data = {0};
            sub->sd->driver->get_bitmaps(sub->sd, res, e->pts, &data);
            copy_sub_bitmaps(&e->data, &data);

            pthread_mutex_unlock(&sub->sd_lock);
            pthread_mutex_lock(&sub->state_lock);
            e->rendered = true;
            cache_entry_unref(e);
            // Potentially wakeup VO thread waiting on this
            pthread_cond_broadcast(&sub->state_wakeup);
        } else {
            pthread_cond_wait(&sub->state_wakeup, &sub->state_lock);
        }
    }
    pthread_mutex_unlock(&sub->state_lock);

    return NULL;
}

static void flush_cache(struct dec_sub *sub)
{
    for (int n = 0; n < sub->num_entries; n++)
        cache_entry_unref(sub->entries[n]);
    sub->num_entries = 0;
    cache_entry_unref(sub->cur);
    sub->cur = NULL;
}

static void flush_packets(struct dec_sub *sub)
{
    for (int n = 0; n < sub->num_packets; n++)
        talloc_free(sub->packets[n]);
    sub->num_packets = 0;
}

void sub_destroy(struct dec_sub *sub)
{
    if (!sub)
        return;
    if (sub->threaded) {
        pthread_mutex_lock(&sub->state_lock);
        sub->threaded = false;
        pthread_cond_broadcast(&sub->state_wakeup);
        pthread_mutex_unlock(&sub->state_lock);
        pthread_join(sub->threaded, NULL);
    }
    flush_cache(sub);
    flush_packets(sub);
    if (sub->sd) {
        sub_reset(sub);
        sub->sd->driver->uninit(sub->sd);
    }
    talloc_free(sub->sd);
    talloc_free(sub->packets);
    pthread_mutex_destroy(&sub->state_lock);
    pthread_cond_destroy(&sub->state_wakeup);
    pthread_mutex_destroy(&sub->sd_lock);
    talloc_free(sub);
}

// Thread-safety of the returned object: all functions are thread-safe,
// except sub_get_bitmaps() and sub_get_text(). Decoder backends (sd_*)
// do not need to acquire locks.
struct dec_sub *sub_create(struct mpv_global *global, struct demuxer *demuxer,
                           struct sh_stream *sh)
{
    assert(demuxer && sh && sh->type == STREAM_SUB);

    struct mp_log *log = mp_log_new(NULL, global->log, "sub");

    for (int n = 0; sd_list[n]; n++) {
        const struct sd_functions *driver = sd_list[n];
        struct dec_sub *sub = talloc_zero(NULL, struct dec_sub);
        sub->log = talloc_steal(sub, log),
        sub->opts = global->opts;
        sub->sh = sh;
        sub->last_pkt_pts = MP_NOPTS_VALUE;
        pthread_mutex_init(&sub->state_lock, NULL);
        pthread_cond_init(&sub->state_wakeup, NULL);
        pthread_mutex_init(&sub->sd_lock, NULL);

        sub->sd = talloc(NULL, struct sd);
        *sub->sd = (struct sd){
            .global = global,
            .log = mp_log_new(sub->sd, sub->log, driver->name),
            .opts = sub->opts,
            .driver = driver,
            .demuxer = demuxer,
            .codec = sh->codec,
        };

        if (sh->codec && sub->sd->driver->init(sub->sd) >= 0) {
            if (sub->opts->sub_render_ahead && !sub->sd->driver->accepts_packet)
            {
                sub->threaded = true;
                if (pthread_create(&sub->thread, NULL, sub_thread, sub))
                    sub->threaded = false;
            }
            return sub;
        }

        ta_set_parent(log, NULL);
        talloc_free(sub->sd);
        sub->sd = NULL;
        sub_destroy(sub);
    }

    mp_err(log, "Could not find subtitle decoder for format '%s'.\n",
           sh->codec->codec);
    talloc_free(log);
    return NULL;
}

static void feed_packet(struct dec_sub *sub, struct demux_packet *pkt)
{
    if (sub->threaded) {
        MP_TARRAY_APPEND(NULL, sub->packets, sub->num_packets, pkt);
    } else {
        sub->sd->driver->decode(sub->sd, pkt);
        talloc_free(pkt);
    }
}

// Read all packets from the demuxer and decode/add them. Returns false if
// there are circumstances which makes this not possible.
bool sub_read_all_packets(struct dec_sub *sub)
{
    pthread_mutex_lock(&sub->state_lock);

    if (sub->sd->driver->accepts_packet) {
        pthread_mutex_unlock(&sub->state_lock);
        return false;
    }

    for (;;) {
        struct demux_packet *pkt = demux_read_packet(sub->sh);
        if (!pkt)
            break;
        feed_packet(sub, pkt);
    }

    sub->preloaded = true;

    pthread_cond_broadcast(&sub->state_wakeup);
    pthread_mutex_unlock(&sub->state_lock);
    return true;
}

// Read packets from the demuxer stream passed to sub_create(). Return true if
// enough packets were read, false if the player should wait until the demuxer
// signals new packets available (and then should retry).
// This can also be used to render subtitles with the given timestamp ahead.
// (Then it's assumed that the player will want to render subtitles at this
// point.)
bool sub_read_packets(struct dec_sub *sub, double video_pts)
{
    bool r = true;
    pthread_mutex_lock(&sub->state_lock);
    while (1) {
        if (sub->preloaded)
            break;
        if (sub->sd->driver->accepts_packet &&
            !sub->sd->driver->accepts_packet(sub->sd))
            break;

        struct demux_packet *pkt;
        int st = demux_read_packet_async(sub->sh, &pkt);
        // Note: "wait" (st==0) happens with non-interleaved streams only, and
        // then we should stop the playloop until a new enough packet has been
        // seen (or the subtitle decoder's queue is full). This does not happen
        // for interleaved subtitle streams, which never return "wait" when
        // reading.
        if (st <= 0) {
            r = st < 0 || (sub->last_pkt_pts != MP_NOPTS_VALUE &&
                           sub->last_pkt_pts > video_pts);
            break;
        }

        sub->last_pkt_pts = pkt->pts;
        feed_packet(sub, pkt);
    }
    if (sub->threaded && r) {
        // xxx: can overflow if either
        // - static readahead count is greater than MAX_BUFFER
        // - VO is somehow not rendering subs
        assert(sub->num_entries < MAX_BUFFER);
        if (!sub->num_entries ||
            video_pts > sub->entries[sub->num_entries - 1]->pts)
        {
            struct cache_entry *e = talloc_ptrtype(NULL, e);
            *e = (struct cache_entry){
                .pts = video_pts,
                .refcount = 1,
            };
            sub->entries[sub->num_entries++] = e;
        }
        // process packets, render ahead subtitles
        pthread_cond_broadcast(&sub->state_wakeup);
    }
    pthread_mutex_unlock(&sub->state_lock);
    return r;
}

// Warning: you must call sub_release_bitmaps() when done. sub_get_bitmaps()
// will block until the previous one has been released.
void sub_get_bitmaps(struct dec_sub *sub, struct mp_osd_res dim, double pts,
                     struct sub_bitmaps *res)
{
    *res = (struct sub_bitmaps) {0};

    if (sub->threaded) {
        pthread_mutex_lock(&sub->state_lock);
        if (!osd_res_equals(sub->last_osd_res, dim)) {
            sub->last_osd_res = dim;
            pthread_cond_broadcast(&sub->state_wakeup);
        }
        assert(!sub->cur);
        for (int n = 0; n < sub->num_entries; n++) {
            struct cache_entry *e = sub->entries[n];
            if (e->pts < pts) {
                // Assume old entries are not needed anymore.
                cache_entry_unref(e);
                MP_TARRAY_REMOVE_AT(sub->entries, sub->num_entries, n);
                n--;
            } else if (e->pts == pts) {
                e->refcount += 1;
                sub->cur = e;
                break;
            }
        }
        bool done = false;
        if (sub->cur) {
            done = true;
            while (!sub->cur->rendered)
                pthread_cond_wait(&sub->state_wakeup, &sub->state_lock);
            *res = sub->cur->data;
            //MP_WARN(sub, "%f in cache\n", pts);
        }
        pthread_mutex_unlock(&sub->state_lock);
        if (done)
            return;
    }

    MP_WARN(sub, "%f not in cache\n", pts);
    pthread_mutex_lock(&sub->sd_lock);
    assert(!sub->reserved);
    sub->reserved = true;
    sub->sd->driver->get_bitmaps(sub->sd, dim, pts, res);
}

void sub_release_bitmaps(struct dec_sub *sub)
{
    if (sub->threaded) {
        pthread_mutex_lock(&sub->state_lock);
        bool had_sub = !!sub->cur;
        if (sub->cur)
            cache_entry_unref(sub->cur);
        sub->cur = NULL;
        pthread_mutex_unlock(&sub->state_lock);
        if (had_sub)
            return;
    }

    assert(sub->reserved);
    sub->reserved = false;
    pthread_mutex_unlock(&sub->sd_lock);
}

// See sub_get_bitmaps() for locking requirements.
// It can be called unlocked too, but then only 1 thread must call this function
// at a time (unless exclusive access is guaranteed).
char *sub_get_text(struct dec_sub *sub, double pts)
{
    pthread_mutex_lock(&sub->sd_lock);
    struct MPOpts *opts = sub->opts;
    char *text = NULL;
    if (opts->sub_visibility && sub->sd->driver->get_text)
        text = sub->sd->driver->get_text(sub->sd, pts);
    pthread_mutex_unlock(&sub->sd_lock);
    return text;
}

void sub_reset(struct dec_sub *sub)
{
    pthread_mutex_lock(&sub->state_lock);
    pthread_mutex_lock(&sub->sd_lock);
    if (sub->sd && sub->sd->driver->reset)
        sub->sd->driver->reset(sub->sd);
    pthread_mutex_unlock(&sub->sd_lock);
    sub->last_pkt_pts = MP_NOPTS_VALUE;
    flush_cache(sub);
    flush_packets(sub);
    pthread_mutex_unlock(&sub->state_lock);
}

void sub_select(struct dec_sub *sub, bool selected)
{
    pthread_mutex_lock(&sub->sd_lock);
    if (sub->sd->driver->select)
        sub->sd->driver->select(sub->sd, selected);
    pthread_mutex_unlock(&sub->sd_lock);
}

int sub_control(struct dec_sub *sub, enum sd_ctrl cmd, void *arg)
{
    int r = CONTROL_UNKNOWN;
    pthread_mutex_lock(&sub->sd_lock);
    if (sub->sd->driver->control)
        r = sub->sd->driver->control(sub->sd, cmd, arg);
    pthread_mutex_unlock(&sub->sd_lock);
    return r;
}

void sub_set_video_fmt(struct dec_sub *sub, struct mp_image_params *fmt)
{
    pthread_mutex_lock(&sub->state_lock);
    if (!mp_image_params_equal(&sub->last_video_fmt, fmt)) {
        sub->last_video_fmt = *fmt;
        sub_control(sub, SD_CTRL_SET_VIDEO_PARAMS, &sub->last_video_fmt);
    }
    pthread_mutex_unlock(&sub->state_lock);
}

