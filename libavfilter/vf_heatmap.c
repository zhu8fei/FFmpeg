
#include "formats.h"
#include "internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "heatmap.h"
#include "libswscale/swscale.h"

/**
 * 配置文件代码:::
 */
typedef struct HeatPoints {
    size_t max, count, p; // 最大可用点数, 当前已经存点数 , 当前画到的点数.
    float *x, *y; // 点坐标
    long *t; // 点时间
} HeatPoints;

typedef struct Conf {
    size_t max, count;//  最大可用帧数, 当前已经存帧数
    long *t;
} Conf;

static HeatPoints *init_points() {
    HeatPoints *points = (HeatPoints *) malloc(sizeof(HeatPoints));
    points->x = (int *) calloc(128, sizeof(int));
    points->y = (int *) calloc(128, sizeof(int));
    points->t = (long *) calloc(128, sizeof(long));
    points->max = 128;
    points->count = 0;
    points->p = 0;
    return points;
}

static int free_points(HeatPoints *points) {
    free(points);
    return 0;
}

static HeatPoints *max_points(HeatPoints *points) {
    size_t tmp = points->max;
    points->max += 128;
    int *px = (int *) calloc(points->max, sizeof(int));
    int *py = (int *) calloc(points->max, sizeof(int));
    long *pt = (long *) calloc(points->max, sizeof(long));
    memcpy(px, points->x, tmp * sizeof(int));
    memcpy(py, points->y, tmp * sizeof(int));
    memcpy(pt, points->t, tmp * sizeof(long));
    points->x = px;
    points->y = py;
    points->t = pt;
    return points;
}

static Conf *init_conf() {
    Conf *conf = (Conf *) malloc(sizeof(Conf));
    conf->t = (long *) calloc(128, sizeof(long));
    conf->max = 128;
    conf->count = 0;
    return conf;

}

static int free_conf(Conf *conf) {
    free(conf);
    return 0;
}

static Conf *max_conf(Conf *conf) {
    size_t tmp = conf->max;
    conf->max += 128;
    long *pt = (long *) calloc(conf->max, sizeof(long));
    memcpy(pt, conf->t, tmp * sizeof(long));
    conf->t = pt;
    return conf;
}

static int formatPoints(HeatPoints *points, long t, char *sp, int w, int h) {
    if (strcmp(sp, ",") == 0) {
        return 0;
    }
    points->t[points->count] = t;
    // 字符解析
    float fx, fy;
    sscanf(sp, "\"x\":\"%f\",\"y\":\"%f\",\"p\":0", &fx, &fy);
    points->x[points->count] = fx;
    points->y[points->count] = fy;
    ++points->count;
    if (points->count == points->max) {
        max_points(points);
    }
    return 0;
}

static int readPoints(char *pointsPath, HeatPoints *points, int w, int h) {
    FILE *file = fopen(pointsPath, "r");
    if (!file) { return 0; }
    char line[800];
    char pl[200];
    long tl;
    char *delim = "{}";
    char *p;
    while ((fscanf(file, "%s", line)) == 1) {
        sscanf(line, "%*[^[][%[^]]],\"st\":%ld}", pl, &tl);
        formatPoints(points, tl, strtok(pl, delim), w, h);
        while ((p = strtok(NULL, delim)))
            formatPoints(points, tl, p, w, h);
    }
    return 0;
}

static int formatConf(Conf *conf, long t) {
    conf->t[conf->count] = t;
    ++conf->count;
    if (conf->count == conf->max) {
        max_conf(conf);
    }
    return 0;
}

static int readConf(char *confPath, Conf *conf) {
    FILE *file = fopen(confPath, "r");
    if (!file) { return 0; }
    long tl;
    while ((fscanf(file, "%*[^=]=%ld", &tl)) == 1) {
        formatConf(conf, tl);
    }
    return 0;
}

static int draw(HeatPoints *points, Conf *conf, int frame, heatmap_t *hm, int w, int h) {
    int last = frame - 1;
    long now = conf->t[last];
    // 当前点小于最大所有点并且 , 当前点对应时间小于当前操作时间时 画下一个点.
    while (points->p < points->count && points->t[points->p] <= now) {
        heatmap_add_point(hm, (unsigned) (w * points->x[points->p]), (unsigned) (h * points->y[points->p]));
        ++points->p;
    }
    return 0;
}

/**
 *  热点滤镜代码:::
 */

enum FilterMode {
    MODE_NONE,
    MODE_INTERLEAVE,
    MODE_DEINTERLEAVE
};

typedef struct HeatmapContext {
    const AVClass *class;
    int nb_inputs;
    HeatPoints *points;
    Conf *conf;
    char *pointsPath, *confPath;
} HeatmapContext;

#define OFFSET(x) offsetof(HeatmapContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

/**
 * 操作符号等内容
 */
static const AVOption heatmap_options[] = {
        {"pointsPath", "set points _c.log file path",        OFFSET(pointsPath), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX,
                FLAGS},
        {"confPath",   "set _timestamp.conf conf file path", OFFSET(confPath),   AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX,
                FLAGS},
        {NULL}
};

AVFILTER_DEFINE_CLASS(heatmap);

static av_cold int init(AVFilterContext *ctx) {
    HeatmapContext *hc = ctx->priv;
    hc->points = init_points();
    hc->conf = init_conf();
    return 0;
}

/**
 * 查询流信息
 * @param ctx
 * @return
 */
static int query_formats(AVFilterContext *ctx) {
    HeatmapContext *hc = ctx->priv;
    AVFilterFormats * formats = NULL;
    int fmt, ret;
    for (fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        if (!(desc->flags & AV_PIX_FMT_FLAG_PAL) &&
            !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) &&
            (ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    readPoints(hc->pointsPath, hc->points, ctx->inputs[0]->w, ctx->inputs[0]->h);
    readConf(hc->confPath, hc->conf);

    return ff_set_common_formats(ctx, formats);
}

static int filter_frame(AVFilterLink *link, AVFrame *frame) {
    AVFilterContext *ctx = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    HeatmapContext *hc = ctx->priv;
    if (hc->points->count == 0) {
        return ff_filter_frame(outlink, frame);
    }

    const size_t w = frame->width, h = frame->height;
    const size_t image_size = w * h * 4;
    unsigned char *image = (unsigned char *) malloc(image_size * sizeof(unsigned char));

    struct HeatmapContext *yuv2rgba = sws_getContext(w, h,
                                                     AV_PIX_FMT_YUV420P, w, h,
                                                     AV_PIX_FMT_RGBA, 0, 0, 0, 0);

    uint8_t *rgbaData[1] = {image}; // RGB24 have one plane
    int rgbaLinesize[1] = {4 * w}; // RGB stride
    sws_scale(yuv2rgba, frame->data, frame->linesize, 0, h, rgbaData, rgbaLinesize);

    heatmap_t *hm = heatmap_new(w, h);

    draw(hc->points, hc->conf, link->frame_count_in, hm, w, h);

    heatmap_render_default_to(hm, image);
    heatmap_free(hm);
    struct HeatmapContext *rgba2yuv = sws_getContext(w, h,
                                                     AV_PIX_FMT_RGBA, w, h,
                                                     AV_PIX_FMT_YUV420P, 0, 0, 0, 0);

    sws_scale(rgba2yuv, rgbaData, rgbaLinesize, 0, h, frame->data, frame->linesize);
    free(image);

    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad inputs[] = {
        {
                .name = "default",
                .type = AVMEDIA_TYPE_VIDEO,
                .filter_frame = filter_frame,
        },
        {NULL}
};
static const AVFilterPad outputs[] = {
        {
                .name = "default",
                .type = AVMEDIA_TYPE_VIDEO,
        },
        {NULL}
};

AVFilter ff_vf_heatmap = {
        .name             = "heatmap",
        .description      = NULL_IF_CONFIG_SMALL(" heat map "),
        .priv_size        = sizeof(HeatmapContext),
        .query_formats    = query_formats,
        .init             = init,
        .inputs           = inputs,
        .outputs          = outputs,
        .priv_class       = &heatmap_class,
        .flags            = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
