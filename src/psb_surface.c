/*
 * Copyright (c) 2007 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <wsbm/wsbm_manager.h>

#include "psb_def.h"
#include "psb_surface.h"

#include <RAR/rar.h>


/*
 * Create surface
 */
VAStatus psb_surface_create( psb_driver_data_p driver_data,
                             int width, int height, int fourcc, int protected,
                             psb_surface_p psb_surface /* out */
                             )
{
    int ret;

    if (fourcc == VA_FOURCC_NV12) 
    {
        if ((width <= 0) || (width > 4096) || (height <= 0) || (height > 4096))
        {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
	    
        if (0)
        {
            ;
        }
        else if (512 >= width)
        {
            psb_surface->stride_mode = STRIDE_512;
            psb_surface->stride = 512;
        }
        else if (1024 >= width)
        {
            psb_surface->stride_mode = STRIDE_1024;
            psb_surface->stride = 1024;
        }
        else if (1280 >= width)
        {
            psb_surface->stride_mode = STRIDE_1280;
            psb_surface->stride = 1280;
        }
        else if (2048 >= width)
        {
            psb_surface->stride_mode = STRIDE_2048;
            psb_surface->stride = 2048;
        }
        else if (4096 >= width)
        {
            psb_surface->stride_mode = STRIDE_4096;
            psb_surface->stride = 4096;
        }
        else
        {
            return VA_STATUS_ERROR_UNKNOWN;
        }
	    
        psb_surface->luma_offset = 0;
        psb_surface->chroma_offset = psb_surface->stride * height;
        psb_surface->size = (psb_surface->stride * height * 3) / 2;
    }
    else if (fourcc == VA_FOURCC_RGBA)  
    {
        unsigned int pitchAlignMask = 63;
        psb_surface->stride_mode = STRIDE_NA;
        psb_surface->stride = (width * 4 + pitchAlignMask) & ~pitchAlignMask;;
        psb_surface->luma_offset = 0;
        psb_surface->chroma_offset = 0;
        psb_surface->size = psb_surface->stride * height;
    }
	
    if (protected == 0)
        ret = psb_buffer_create( driver_data, psb_surface->size, psb_bt_surface, &psb_surface->buf );
    else
        ret = psb_buffer_create_rar( driver_data, psb_surface->size, &psb_surface->buf );
        
    return ret ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_SUCCESS;
}

/* id_or_ofs: it is frame ID or frame offset in camear device memory
 *     for CI frame: it it always frame offset currently
 *     for v4l2 buf: it is offset used in V4L2 buffer mmap
 */
VAStatus psb_surface_create_camera( psb_driver_data_p driver_data,
                             int width, int height, int stride, int size,
                             psb_surface_p psb_surface, /* out */
                             int is_v4l2,
                             unsigned int id_or_ofs
                             )
{
    int ret;

    if ((width <= 0) || (width > 4096) || (height <= 0) || (height > 4096))
    {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    
    psb_surface->stride = stride; 
    psb_surface->chroma_offset = psb_surface->stride * height;
    psb_surface->size = (psb_surface->stride * height * 3) / 2;

    ret = psb_buffer_create_camera(driver_data,&psb_surface->buf,
                                   is_v4l2, id_or_ofs);
    
    if (ret != VA_STATUS_SUCCESS) {
        psb_surface_destroy(psb_surface);
        
        psb__error_message("Get surfae offset of camear device memory failed!\n");
        return ret;
    }

    return VA_STATUS_SUCCESS;
}


/*
 * Temporarily map surface and set all chroma values of surface to 'chroma'
 */   
VAStatus psb_surface_set_chroma( psb_surface_p psb_surface, int chroma )
{
    void *surface_data;
    int ret = psb_buffer_map(&psb_surface->buf, &surface_data);
    
    if (ret) return VA_STATUS_ERROR_UNKNOWN;

    memset(surface_data + psb_surface->chroma_offset, chroma, psb_surface->size - psb_surface->chroma_offset);
    
    psb_buffer_unmap(&psb_surface->buf);

    return VA_STATUS_SUCCESS;
}

/*
 * Destroy surface
 */   
void psb_surface_destroy( psb_surface_p psb_surface )
{
    psb_buffer_destroy( &psb_surface->buf );
}

VAStatus psb_surface_sync( psb_surface_p psb_surface )
{
    wsbmBOWaitIdle(psb_surface->buf.drm_buf, 0);
    
    return VA_STATUS_SUCCESS;
}

VAStatus psb_surface_query_status( psb_surface_p psb_surface, VASurfaceStatus *status )
{
    int ret;
    uint32_t synccpu_flag = WSBM_SYNCCPU_READ | WSBM_SYNCCPU_WRITE | WSBM_SYNCCPU_DONT_BLOCK;
    
    ret = wsbmBOSyncForCpu(psb_surface->buf.drm_buf, synccpu_flag);
    (void) wsbmBOReleaseFromCpu(psb_surface->buf.drm_buf, synccpu_flag);
    
    if (ret == 0)
        *status = VASurfaceReady;
    else
        *status = VASurfaceRendering;
        
    return VA_STATUS_SUCCESS;
}
