/*
 * Filter graphs to bad ASCII-art
 * Copyright (c) 2012 Nicolas George
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/bprint.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "internal.h"

#define GRAPHDUMP_TO_DOT 1

static int print_link_prop(AVBPrint *buf, AVFilterLink *link)
{
    char *format;
    char layout[64];
    AVBPrint dummy_buffer = { 0 };

    if (!buf)
        buf = &dummy_buffer;
    switch (link->type) {
        case AVMEDIA_TYPE_VIDEO:
            format = av_x_if_null(av_get_pix_fmt_name(link->format), "?");
#ifdef GRAPHDUMP_TO_DOT
            av_bprintf(buf, "resolution:%dx%d\\nSAR:%d:%d\\nFormat:%s\\nInterlaced:%d\\nTFF:%d\\nTimebase:%d/%d\\nFramerate:%d/%d\n",
                       link->w, link->h,
                       link->sample_aspect_ratio.num,
                       link->sample_aspect_ratio.den,
                       format,
                       link->interlaced_frame,
                       link->top_field_first,
                       link->time_base.num, link->time_base.den,
                       link->frame_rate.num, link->frame_rate.den);
#else
            av_bprintf(buf, "[%dx%d %d:%d %s]", link->w, link->h,
                    link->sample_aspect_ratio.num,
                    link->sample_aspect_ratio.den,
                    format);
#endif
            break;

        case AVMEDIA_TYPE_AUDIO:
            av_get_channel_layout_string(layout, sizeof(layout),
                                         link->channels, link->channel_layout);
            format = av_x_if_null(av_get_sample_fmt_name(link->format), "?");
            av_bprintf(buf, "[%dHz %s:%s]",
                       (int)link->sample_rate, format, layout);
            break;

        default:
            av_bprintf(buf, "?");
            break;
    }
    return buf->len;
}
#ifdef GRAPHDUMP_TO_DOT
 static void avfilter_graph_dump_to_buf(AVBPrint *buf, AVFilterGraph *graph)
 {
     unsigned i, j, x, e;

     for (i = 0; i < graph->nb_filters; i++) {
         AVFilterContext *filter = graph->filters[i];

        /* First print properties of filter */
        av_bprintf(buf, "subgraph \"cluster_%p\"\n{\n\tlabel=\"%s\\n(%s)\"\n\trankdir=LR\n", filter, filter->name, filter->filter->name);
        av_bprintf(buf, "subgraph \"cluster_inputs\"\n{\n\tlabel=\"inputs\"\nstyle=\"invis\"\n");
        for (int j = 0; j < filter->nb_inputs; j++) {
            av_bprintf(buf, "\t\"%p\" [label=\"%s\", color=lightpink2]\n", filter->inputs[j]->srcpad, avfilter_pad_get_name(filter->input_pads, j));
        }
        av_bprintf(buf, "}\n");
        av_bprintf(buf, "subgraph \"cluster_outputs\"\n{\n\tlabel=\"outputs\"\nstyle=\"invis\"\n");
        for (int j = 0; j < filter->nb_outputs; j++) {
            av_bprintf(buf, "\t\"%p\" [label=\"%s\", color=lightblue2]\n", filter->outputs[j]->dstpad, avfilter_pad_get_name(filter->output_pads, j));
        }
        av_bprintf(buf, "}\n");
        /* Draw an invisible link between the sink and source to impact layout */
        for (int j = 0; j < filter->nb_inputs && j < filter->nb_outputs; j++) {
            av_bprintf(buf, "\t\"%p\" -> \"%p\" [style=\"invis\"]\n", filter->inputs[j]->srcpad, filter->outputs[j]->dstpad);
        }
        av_bprintf(buf, "}\n");
    }
    for (i = 0; i < graph->nb_filters; i++) {
        AVFilterContext *filter = graph->filters[i];

        /* Show links */
        for (j = 0; j < filter->nb_inputs; j++) {
            AVFilterLink *l = filter->inputs[j];
            av_bprintf(buf, "\t\"%p\" -> \"%p\" [label=\"", l->dstpad, l->srcpad);
            print_link_prop(buf, l);
            av_bprintf(buf, "\"]\n");
        }
    }
}
#else
static void avfilter_graph_dump_to_buf(AVBPrint *buf, AVFilterGraph *graph)
{
    unsigned i, j, x, e;

    for (i = 0; i < graph->nb_filters; i++) {
        AVFilterContext *filter = graph->filters[i];
        unsigned max_src_name = 0, max_dst_name = 0;
        unsigned max_in_name  = 0, max_out_name = 0;
        unsigned max_in_fmt   = 0, max_out_fmt  = 0;
        unsigned width, height, in_indent;
        unsigned lname = strlen(filter->name);
        unsigned ltype = strlen(filter->filter->name);

        for (j = 0; j < filter->nb_inputs; j++) {
            AVFilterLink *l = filter->inputs[j];
            unsigned ln = strlen(l->src->name) + 1 + strlen(l->srcpad->name);
            max_src_name = FFMAX(max_src_name, ln);
            max_in_name = FFMAX(max_in_name, strlen(l->dstpad->name));
            max_in_fmt = FFMAX(max_in_fmt, print_link_prop(NULL, l));
        }
        for (j = 0; j < filter->nb_outputs; j++) {
            AVFilterLink *l = filter->outputs[j];
            unsigned ln = strlen(l->dst->name) + 1 + strlen(l->dstpad->name);
            max_dst_name = FFMAX(max_dst_name, ln);
            max_out_name = FFMAX(max_out_name, strlen(l->srcpad->name));
            max_out_fmt = FFMAX(max_out_fmt, print_link_prop(NULL, l));
        }
        in_indent = max_src_name + max_in_name + max_in_fmt;
        in_indent += in_indent ? 4 : 0;
        width = FFMAX(lname + 2, ltype + 4);
        height = FFMAX3(2, filter->nb_inputs, filter->nb_outputs);
        av_bprint_chars(buf, ' ', in_indent);
        av_bprintf(buf, "+");
        av_bprint_chars(buf, '-', width);
        av_bprintf(buf, "+\n");
        for (j = 0; j < height; j++) {
            unsigned in_no  = j - (height - filter->nb_inputs ) / 2;
            unsigned out_no = j - (height - filter->nb_outputs) / 2;

            /* Input link */
            if (in_no < filter->nb_inputs) {
                AVFilterLink *l = filter->inputs[in_no];
                e = buf->len + max_src_name + 2;
                av_bprintf(buf, "%s:%s", l->src->name, l->srcpad->name);
                av_bprint_chars(buf, '-', e - buf->len);
                e = buf->len + max_in_fmt + 2 +
                    max_in_name - strlen(l->dstpad->name);
                print_link_prop(buf, l);
                av_bprint_chars(buf, '-', e - buf->len);
                av_bprintf(buf, "%s", l->dstpad->name);
            } else {
                av_bprint_chars(buf, ' ', in_indent);
            }

            /* Filter */
            av_bprintf(buf, "|");
            if (j == (height - 2) / 2) {
                x = (width - lname) / 2;
                av_bprintf(buf, "%*s%-*s", x, "", width - x, filter->name);
            } else if (j == (height - 2) / 2 + 1) {
                x = (width - ltype - 2) / 2;
                av_bprintf(buf, "%*s(%s)%*s", x, "", filter->filter->name,
                        width - ltype - 2 - x, "");
            } else {
                av_bprint_chars(buf, ' ', width);
            }
            av_bprintf(buf, "|");

            /* Output link */
            if (out_no < filter->nb_outputs) {
                AVFilterLink *l = filter->outputs[out_no];
                unsigned ln = strlen(l->dst->name) + 1 +
                              strlen(l->dstpad->name);
                e = buf->len + max_out_name + 2;
                av_bprintf(buf, "%s", l->srcpad->name);
                av_bprint_chars(buf, '-', e - buf->len);
                e = buf->len + max_out_fmt + 2 +
                    max_dst_name - ln;
                print_link_prop(buf, l);
                av_bprint_chars(buf, '-', e - buf->len);
                av_bprintf(buf, "%s:%s", l->dst->name, l->dstpad->name);
            }
            av_bprintf(buf, "\n");
        }
        av_bprint_chars(buf, ' ', in_indent);
        av_bprintf(buf, "+");
        av_bprint_chars(buf, '-', width);
        av_bprintf(buf, "+\n");
        av_bprintf(buf, "\n");
    }
}
#endif

char *avfilter_graph_dump(AVFilterGraph *graph, const char *options)
{
    AVBPrint buf;
    char *dump;

    av_bprint_init(&buf, 0, 0);
    avfilter_graph_dump_to_buf(&buf, graph);
    av_bprint_init(&buf, buf.len + 1, buf.len + 1);
    avfilter_graph_dump_to_buf(&buf, graph);
    av_bprint_finalize(&buf, &dump);
    return dump;
}
