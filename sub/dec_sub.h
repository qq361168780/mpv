#ifndef MPLAYER_DEC_SUB_H
#define MPLAYER_DEC_SUB_H

#include <stdbool.h>
#include <stdint.h>

#include "osd.h"

struct demuxer;
struct sh_stream;
struct mpv_global;
struct mp_image_params;

struct dec_sub;
struct sd;

enum sd_ctrl {
    SD_CTRL_SUB_STEP,
    SD_CTRL_SET_VIDEO_PARAMS,
    SD_CTRL_SET_TOP,
    SD_CTRL_SET_VIDEO_DEF_FPS,
};

#define MAX_SUB_RENDER_AHEAD 50

struct dec_sub *sub_create(struct mpv_global *global, struct demuxer *demuxer,
                           struct sh_stream *sh);
void sub_destroy(struct dec_sub *sub);

bool sub_read_all_packets(struct dec_sub *sub);
bool sub_read_packets(struct dec_sub *sub, double video_pts);
void sub_add_pts(struct dec_sub *sub, double pts);
void sub_get_bitmaps(struct dec_sub *sub, struct mp_osd_res dim, double pts,
                     struct sub_bitmaps *res);
void sub_release_bitmaps(struct dec_sub *sub);
char *sub_get_text(struct dec_sub *sub, double pts);
void sub_reset(struct dec_sub *sub);
void sub_select(struct dec_sub *sub, bool selected);

int sub_control(struct dec_sub *sub, enum sd_ctrl cmd, void *arg);
void sub_set_video_fmt(struct dec_sub *sub, struct mp_image_params *fmt);

#endif
