/*
 * NewTek NDI common code
 * Copyright (c) 2018 Maksym Veremeyenko
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

#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "libndi_newtek_common.h"

#define NDI_LIB_LOAD_ERROR_TEXT "\nPlease re-install the NewTek NDI Runtimes from " NDILIB_REDIST_URL " to use this functionality."

const NDIlib_v3* ndi_lib_load(AVFormatContext *avctx) {
    char *path = NULL, *e;
    const NDIlib_v3* (*NDIlib_v3_load)(void) = NULL;
#ifdef _WIN32
    HMODULE
#else
    void*
#endif
        hNDILib;

    e = getenv(NDILIB_REDIST_FOLDER);
    if (!e) {
        path = av_strdup(NDILIB_LIBRARY_NAME);
        if (!path)
            return NULL;
    }
    else {
        int s = strlen(NDILIB_LIBRARY_NAME) + 1 + strlen(e) + 1;
        path = av_malloc(s);
        if (!path)
            return NULL;
        snprintf(path, s, "%s"
#ifdef _WIN32
            "\\"
#else
            "/"
#endif
            "%s", e, NDILIB_LIBRARY_NAME);
    }


#ifdef _WIN32
    /* Try to load the library */
    hNDILib = LoadLibrary(path);

    if (!hNDILib)
        av_log(avctx, AV_LOG_ERROR, "LoadLibrary(%s) failed. " NDI_LIB_LOAD_ERROR_TEXT "\n", path);
    else {

        /* get NDIlib_v3_load address */
        *((FARPROC*)&NDIlib_v3_load) = GetProcAddress(hNDILib, "NDIlib_v3_load");

        if (!NDIlib_v3_load) {
            av_log(avctx, AV_LOG_ERROR, "GetProcAddress(NDIlib_v3_load) failed in file [%s]. " NDI_LIB_LOAD_ERROR_TEXT "\n", path);
            FreeLibrary(hNDILib);
        }
    }
#else
    /* Try to load the library */
    hNDILib = dlopen(path, RTLD_LOCAL | RTLD_LAZY);

    if (!hNDILib)
        av_log(avctx, AV_LOG_ERROR, "dlopen(%s) failed. " NDI_LIB_LOAD_ERROR_TEXT "\n", path);
    else {

        /* get NDIlib_v3_load address */
        *((void**)&NDIlib_v3_load) = dlsym(hNDILib, "NDIlib_v3_load");

        if (!NDIlib_v3_load) {
            av_log(avctx, AV_LOG_ERROR, "dlsym(NDIlib_v3_load) failed in file[%s]. " NDI_LIB_LOAD_ERROR_TEXT "\n", path);
            dlclose(hNDILib);
        }
    }
#endif

    av_free(path);

    return NDIlib_v3_load ? NDIlib_v3_load() : NULL;
}
