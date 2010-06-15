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
#include <va/va_backend.h>
#include <va/va_backend_tpi.h>
#ifndef ANDROID
#include <va/va_dri.h>
#include <va/va_dri2.h>
#endif
#include <va/va_dricommon.h>

#include "psb_drv_video.h"
#include "psb_cmdbuf.h"
#include "lnc_cmdbuf.h"
#include "pnw_cmdbuf.h"
#include "psb_surface.h"
#include "psb_MPEG2.h"
#include "psb_MPEG4.h"
#include "psb_H264.h"
#include "psb_VC1.h"
#include "pnw_MPEG2.h"
#include "pnw_MPEG4.h"
#include "pnw_H264.h"
#include "lnc_MPEG4ES.h"
#include "lnc_H264ES.h"
#include "lnc_H263ES.h"
#include "pnw_MPEG4ES.h"
#include "pnw_H264ES.h"
#include "pnw_H263ES.h"
#include "pnw_jpeg.h"
#include "psb_output.h"
#include "lnc_ospm_event.h"
#ifndef ANDROID 
#include <X11/Xutil.h>
#include <X11/Xlibint.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#else
#include "android/psb_android_glue.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <wsbm/wsbm_pool.h>
#include <wsbm/wsbm_manager.h>
#include <wsbm/wsbm_util.h>
#include <wsbm/wsbm_fencemgr.h>
#include <linux/videodev2.h>

#include "psb_dri.h"

#include "psb_def.h"
#include "psb_ws_driver.h"
#include "ci_va.h"

#ifndef PSB_PACKAGE_VERSION
#define PSB_PACKAGE_VERSION "Undefined"
#endif

#define PSB_DRV_VERSION  PSB_PACKAGE_VERSION
#define PSB_CHG_REVISION "(0X00000023)"


#define PSB_STR_VENDOR	"Intel GMA500-" PSB_DRV_VERSION " " PSB_CHG_REVISION

#define MAX_UNUSED_BUFFERS	16

#define PSB_MAX_FLIP_DELAY (1000/30/10)

#ifdef DEBUG_TRACE
#include <signal.h>
#endif

#define EXPORT __attribute__ ((visibility("default")))

#define INIT_DRIVER_DATA    psb_driver_data_p driver_data = (psb_driver_data_p) ctx->pDriverData;
#define INIT_FORMAT_VTABLE format_vtable_p format_vtable = (((profile >= 0) && (profile < PSB_MAX_PROFILES)) && (entrypoint >=0 && (entrypoint < PSB_MAX_ENTRYPOINTS))) ? driver_data->profile2Format[profile][entrypoint] : NULL;

#define CONFIG(id)  ((object_config_p) object_heap_lookup( &driver_data->config_heap, id ))
#define CONTEXT(id) ((object_context_p) object_heap_lookup( &driver_data->context_heap, id ))
#define SURFACE(id)    ((object_surface_p) object_heap_lookup( &driver_data->surface_heap, id ))
#define BUFFER(id)  ((object_buffer_p) object_heap_lookup( &driver_data->buffer_heap, id ))

#define CONFIG_ID_OFFSET        0x01000000
#define CONTEXT_ID_OFFSET       0x02000000
#define SURFACE_ID_OFFSET       0x03000000
#define BUFFER_ID_OFFSET        0x04000000
#define IMAGE_ID_OFFSET         0x05000000
#define SUBPIC_ID_OFFSET        0x06000000

static int psb_get_device_info( VADriverContextP ctx );

void psb__error_message(const char *msg, ...)
{
    va_list args;

    fprintf(stderr, "psb_drv_video error: ");
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
}

void psb__information_message(const char *msg, ...)
{
    static int psb_video_debug = -1;
    if (psb_video_debug == -1)
    {
        psb_video_debug = (getenv("PSB_VIDEO_DEBUG") != NULL);
    }
    if (psb_video_debug)
    {
        va_list args;

        fprintf(stderr, "psb_drv_video: ");
        va_start(args, msg);
        vfprintf(stderr, msg, args);
        va_end(args);
    }
}

#ifdef DEBUG_TRACE
void psb__trace_message(const char *msg, ...)
{
    va_list args;
    static const char *trace_file = 0;
    static FILE *trace = 0;
    
    if (!trace_file)
    {
        trace_file = getenv("PSB_VIDEO_TRACE");
        if (trace_file)
        {
            trace = fopen(trace_file, "w");
            if (trace)
            {
                time_t curtime;
                time(&curtime);
                fprintf(trace, "---- %s\n---- Start Trace ----\n", ctime(&curtime));
            }
        }
        else
        {
            trace_file = "none";
        }
    }
    if (trace)
    {
        if (msg)
        {
            va_start(args, msg);
            vfprintf(trace, msg, args);
            va_end(args);
        }
        else
        {
            fflush(trace);
        }
    }
}
#endif

VAStatus psb_QueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,    /* out */
        int *num_profiles            /* out */
    )
{
    (void) ctx; /* unused */
    int i = 0;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    INIT_DRIVER_DATA
    
    if(NULL == profile_list)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if(NULL == num_profiles)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }

//    profile_list[i++] = VAProfileMPEG2Simple;
    profile_list[i++] = VAProfileMPEG2Main;
    profile_list[i++] = VAProfileMPEG4Simple;
    profile_list[i++] = VAProfileMPEG4AdvancedSimple;
//    profile_list[i++] = VAProfileMPEG4Main;
    profile_list[i++] = VAProfileH264Baseline;
    profile_list[i++] = VAProfileH264Main;
    profile_list[i++] = VAProfileH264High;
    profile_list[i++] = VAProfileVC1Simple;
    profile_list[i++] = VAProfileVC1Main;
    profile_list[i++] = VAProfileVC1Advanced;

    if (IS_MFLD(driver_data))
    {
	profile_list[i++] = VAProfileH263Baseline;
	profile_list[i++] = VAProfileJPEGBaseline;
    }
    else if (IS_MRST(driver_data))
        profile_list[i++] = VAProfileH263Baseline;
    
    /* If the assert fails then PSB_MAX_PROFILES needs to be bigger */
    ASSERT(i <= PSB_MAX_PROFILES);
    *num_profiles = i;
 
    return VA_STATUS_SUCCESS;
}


VAStatus psb_QueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,    /* out */
        int *num_entrypoints        /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    format_vtable_p * format_vtable = ((profile >= 0) && (profile < PSB_MAX_PROFILES)) ? driver_data->profile2Format[profile] : NULL;
    int entrypoints = 0;

    if(NULL == entrypoint_list)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if(NULL == num_entrypoints)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if (format_vtable)

    {
        int i;

        for (i = 0; i < PSB_MAX_ENTRYPOINTS; i++) {
            if (format_vtable[i]) {
                entrypoints++;
                *entrypoint_list++ = i;
            }
        }
    }

    /* If the assert fails then PSB_MAX_ENTRYPOINTS needs to be bigger */
    ASSERT(entrypoints <= PSB_MAX_ENTRYPOINTS);

    if (0 == entrypoints)
    {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    *num_entrypoints = entrypoints;
    return VA_STATUS_SUCCESS;
}

/*
 * Figure out if we should return VA_STATUS_ERROR_UNSUPPORTED_PROFILE
 * or VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT
 */
static VAStatus psb__error_unsupported_profile_entrypoint( psb_driver_data_p driver_data, VAProfile profile, VAEntrypoint entrypoint)
{
    format_vtable_p * format_vtable = ((profile >= 0) && (profile < PSB_MAX_PROFILES)) ? driver_data->profile2Format[profile] : NULL;

    /* Does the driver support _any_ entrypoint for this profile? */
    if (format_vtable)
    {
        int i;

        for (i = 0; i < PSB_MAX_ENTRYPOINTS; i++) {
            if (format_vtable[i]) {
                /* There is an entrypoint, so the profile is supported */
                return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
            }
        }
    }
    return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

VAStatus psb_GetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,    /* in/out */
        int num_attribs
    )
{
    INIT_DRIVER_DATA
    INIT_FORMAT_VTABLE
    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    if (NULL == format_vtable)
    {
        return psb__error_unsupported_profile_entrypoint(driver_data, profile, entrypoint);
    }
    if(NULL == attrib_list)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    } 
    if (num_attribs <= 0)
    {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    /* Generic attributes */
    for (i = 0; i < num_attribs; i++)
    {
        switch (attrib_list[i].type)
        {
          case VAConfigAttribRTFormat:
              attrib_list[i].value = VA_RT_FORMAT_YUV420;
              break;
              
          default:
              attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
              break;
        }
    }
    /* format specific attributes */
    format_vtable->queryConfigAttributes(profile, entrypoint, attrib_list, num_attribs);

    return VA_STATUS_SUCCESS;
}

static VAStatus psb__update_attribute(object_config_p obj_config, VAConfigAttrib *attrib)
{
    int i;
    /* Check existing attributes */
    for(i = 0; i < obj_config->attrib_count; i++)
    {
        if (obj_config->attrib_list[i].type == attrib->type)
        {
            /* Update existing attribute */
            obj_config->attrib_list[i].value = attrib->value;
            return VA_STATUS_SUCCESS;
        }
    }
    if (obj_config->attrib_count < PSB_MAX_CONFIG_ATTRIBUTES)
    {
        i = obj_config->attrib_count;
        obj_config->attrib_list[i].type = attrib->type;
        obj_config->attrib_list[i].value = attrib->value;
        obj_config->attrib_count++;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus psb__validate_config(object_config_p obj_config)
{
    int i;
    /* Check all attributes */
    for(i = 0; i < obj_config->attrib_count; i++)
    {
        switch (obj_config->attrib_list[i].type)
        {
          case VAConfigAttribRTFormat:
                  if (obj_config->attrib_list[i].value != VA_RT_FORMAT_YUV420)
                  {
                      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
                  }
                  break;
          
          default:
                  /*
                   * Ignore unknown attributes here, it
                   * may be format specific.
                   */
                  break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus psb_CreateConfig(
        VADriverContextP ctx,
        VAProfile profile, 
        VAEntrypoint entrypoint, 
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id        /* out */
    )
{
    INIT_DRIVER_DATA
    INIT_FORMAT_VTABLE
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int configID;
    object_config_p obj_config;
    int i;

    if (num_attribs < 0)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (NULL == format_vtable)
    {
        vaStatus = psb__error_unsupported_profile_entrypoint(driver_data, profile, entrypoint);
    }

    if (VA_STATUS_SUCCESS != vaStatus)
    {
        return vaStatus;
    }

    configID = object_heap_allocate( &driver_data->config_heap );
    obj_config = CONFIG(configID);
    if (NULL == obj_config)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        return vaStatus;
    }

    obj_config->profile = profile;
    obj_config->format_vtable = format_vtable;
    obj_config->entrypoint = entrypoint;
    obj_config->attrib_list[0].type = VAConfigAttribRTFormat;
    obj_config->attrib_list[0].value = VA_RT_FORMAT_YUV420;
    obj_config->attrib_count = 1;

    for(i = 0; i < num_attribs; i++)
    {
	if (attrib_list[i].type < VAConfigAttribRTFormat || attrib_list[i].type > VAConfigAttribRateControl)
	    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;

        vaStatus = psb__update_attribute(obj_config, &(attrib_list[i]));
        if (VA_STATUS_SUCCESS != vaStatus)
        {
            break;
        }
    }

    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = psb__validate_config(obj_config);
    }
    
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = format_vtable->validateConfig(obj_config);
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        object_heap_free( &driver_data->config_heap, (object_base_p) obj_config);
    }
    else
    {
        *config_id = configID;
    }

    return vaStatus;
}

VAStatus psb_DestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;

    obj_config = CONFIG(config_id);
    if (NULL == obj_config)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
        DEBUG_FAILURE;
        return vaStatus;
    }

    object_heap_free( &driver_data->config_heap, (object_base_p) obj_config);
    return vaStatus;
}
    
VAStatus psb_QueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id, 
        VAProfile *profile,        /* out */
        VAEntrypoint *entrypoint,     /* out */
        VAConfigAttrib *attrib_list,    /* out */
        int *num_attribs        /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_config_p obj_config;
    int i;

    if(NULL == profile)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if(NULL == entrypoint)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if(NULL == attrib_list)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    if(NULL == num_attribs)
    {
	vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
	DEBUG_FAILURE;
	return vaStatus;
    }
    obj_config = CONFIG(config_id);
    if (NULL == obj_config)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    *profile = obj_config->profile;
    *entrypoint = obj_config->entrypoint;
    *num_attribs =  obj_config->attrib_count;
    for(i = 0; i < obj_config->attrib_count; i++)
    {
        attrib_list[i] = obj_config->attrib_list[i];
    }
    
    return vaStatus;
}

static void psb__destroy_surface(psb_driver_data_p driver_data, object_surface_p obj_surface)
{
    if (NULL != obj_surface)
    {
        /* delete subpicture association */
        psb_SurfaceDeassociateSubpict(driver_data,obj_surface);
        
        psb_surface_destroy(obj_surface->psb_surface);
        free(obj_surface->psb_surface);
        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
    }
}

static
VAStatus psb__checkSurfaceDimensions(psb_driver_data_p driver_data, int width, int height)
{
    if (driver_data->video_sd_disabled)
    {
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    if ((width <= 0) || (width > 4096) || (height <= 0) || (height > 4096))
    {
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    }
    if (driver_data->video_hd_disabled)
    {
        if ((width > 1024) || (height > 576))
        {
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        }
    }
    
    return VA_STATUS_SUCCESS;
}

static VAStatus psb_register_video_bcd(
        VADriverContextP ctx, 
        int width, 
        int height, 
        int stride, 
        int num_surfaces, 
        VASurfaceID *surface_list
    )
{
    INIT_DRIVER_DATA
    int i;
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    BC_Video_ioctl_package ioctl_package;
    bc_buf_params_t buf_param;

    buf_param.count = num_surfaces;
    buf_param.width = width;
    buf_param.stride = stride;
    buf_param.height = height;
    buf_param.fourcc = BC_PIX_FMT_NV12;
    buf_param.type = BC_MEMORY_USERPTR;
        
    ioctl_package.ioctl_cmd = BC_Video_ioctl_request_buffers;
    ioctl_package.inputparam = (int)(&buf_param);
    if(drmCommandWriteRead(driver_data->drm_fd, DRM_BUFFER_CLASS_VIDEO, &ioctl_package, sizeof(ioctl_package)) != 0)
    {
        LOGE("Failed to request buffers from buffer class video driver (errno=%d).\n", errno);
        return VA_STATUS_ERROR_UNKNOWN;
    }

    ioctl_package.ioctl_cmd = BC_Video_ioctl_get_buffer_count;
    if(drmCommandWriteRead(driver_data->drm_fd, DRM_BUFFER_CLASS_VIDEO, &ioctl_package, sizeof(ioctl_package)) != 0)
    {
        LOGE("Failed to get buffer count from buffer class video driver (errno=%d).\n", errno);
        return VA_STATUS_ERROR_UNKNOWN;
    }

    if (ioctl_package.outputparam != num_surfaces) {
        LOGE("buffer count is not correct (errno=%d).\n", errno);
        return VA_STATUS_ERROR_UNKNOWN;
    }

    bc_buf_ptr_t buf_pa;

    for (i = 0; i < num_surfaces; i++)
    {
        psb_surface_p psb_surface;
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        psb_surface = obj_surface->psb_surface;
        /*get ttm buffer handle*/
        buf_pa.handle = wsbmKBufHandle(wsbmKBuf(psb_surface->buf.drm_buf));
        buf_pa.index = i;
        ioctl_package.ioctl_cmd = BC_Video_ioctl_set_buffer_phyaddr;
        ioctl_package.inputparam = (int) (&buf_pa);
        /*bind bcd buffer index with ttm buffer handle and set buffer phyaddr in kernel driver*/
        if(drmCommandWriteRead(driver_data->drm_fd, DRM_BUFFER_CLASS_VIDEO, &ioctl_package, sizeof(ioctl_package)) != 0)
        {
            LOGE("Failed to set buffer phyaddr from buffer class video driver (errno=%d).\n", errno);
            return VA_STATUS_ERROR_UNKNOWN;
        }
    }
    return vaStatus;
}


VAStatus psb_CreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surface_list        /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int i;
    unsigned int stride = 0;

    if (num_surfaces <= 0)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    if (NULL == surface_list)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    /* We only support one format */
    if ((VA_RT_FORMAT_YUV420 & format) == 0)
    {
        vaStatus = VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        DEBUG_FAILURE;
        return vaStatus;
    }

    vaStatus = psb__checkSurfaceDimensions(driver_data, width, height);
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        DEBUG_FAILURE;
        return vaStatus;
    }

    /* Adjust height to be a multiple of 32 (height of macroblock in interlaced mode) */
    height = (height + 0x1f) & ~0x1f;

    for (i = 0; i < num_surfaces; i++)
    {
        int surfaceID;
        object_surface_p obj_surface;
        psb_surface_p psb_surface;

        surfaceID = object_heap_allocate( &driver_data->surface_heap );
        obj_surface = SURFACE(surfaceID);
        if (NULL == obj_surface)
        {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            break;
        }
        
        obj_surface->surface_id = surfaceID;
        surface_list[i] = surfaceID;
        obj_surface->context_id = -1;
        obj_surface->width = width;
        obj_surface->height = height;
        obj_surface->subpictures = NULL;
        obj_surface->subpic_count = 0; 
        obj_surface->derived_imgcnt = 0;

        psb_surface = (psb_surface_p) malloc(sizeof(struct psb_surface_s));
        if (NULL == psb_surface)
        {
            object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;

            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;

            DEBUG_FAILURE;
            break;
        }
        memset(psb_surface, 0, sizeof(struct psb_surface_s));
        
        vaStatus = psb_surface_create( driver_data, width, height, VA_FOURCC_NV12, 
                                       (VA_RT_FORMAT_PROTECTED & format), psb_surface
                                       );
        
        if ( VA_STATUS_SUCCESS != vaStatus )
        {
            free(psb_surface);
            object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
            obj_surface->surface_id = VA_INVALID_SURFACE;

            DEBUG_FAILURE;
            break;
        }

        stride = psb_surface->stride;
        
	/* by default, surface fourcc is NV12 */
        memset(psb_surface->extra_info, 0, sizeof(psb_surface->extra_info));
        psb_surface->extra_info[4] = VA_FOURCC_NV12;

        obj_surface->psb_surface = psb_surface;
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        /* surface_list[i-1] was the last successful allocation */
        for(; i--; )
        {
            object_surface_p obj_surface = SURFACE(surface_list[i]);
            psb__destroy_surface(driver_data, obj_surface);
            surface_list[i] = VA_INVALID_SURFACE;
        }
    }

#ifdef ANDROID_VIDEO_TEXTURE_STREAM
    vaStatus = psb_register_video_bcd(ctx, width, height, stride, num_surfaces, surface_list);
#endif    

    return vaStatus;
}


VAStatus psb_CreateSurfaceFromCIFrame(
        VADriverContextP ctx,
        unsigned long frame_id,
        VASurfaceID *surface        /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int surfaceID;
    object_surface_p obj_surface;
    psb_surface_p psb_surface;
    struct ci_frame_info frame_info;
    int ret = 0, fd = -1;
    char *camera_dev = NULL;
    
    if (IS_MRST(driver_data) == 0)
        return VA_STATUS_ERROR_UNKNOWN;

    camera_dev = getenv("PSB_VIDEO_CAMERA_DEVNAME");
    
    if (camera_dev)
        fd = open_device(camera_dev);
    else
        fd = open_device("/dev/video0");
    if (fd == -1)
        return  VA_STATUS_ERROR_UNKNOWN;

    frame_info.frame_id = frame_id;
    ret = ci_get_frame_info(fd, &frame_info);
    close_device(fd);
    
    if (ret != 0) 
        return  VA_STATUS_ERROR_UNKNOWN;

    psb__information_message("CI Frame: id=0x%08x, %dx%d, stride=%d, offset=0x%08x, fourcc=0x%08x\n",
                             frame_info.frame_id, frame_info.width, frame_info.height,
                             frame_info.stride, frame_info.offset, frame_info.fourcc);

    if (frame_info.stride & 0x3f) {
        psb__error_message("CI Frame must be 64byte aligned!\n");
        /* return  VA_STATUS_ERROR_UNKNOWN; */
    }

    if (frame_info.fourcc != VA_FOURCC_NV12) {
        psb__error_message("CI Frame must be NV12 format!\n");
        return  VA_STATUS_ERROR_UNKNOWN;
    }
    if (frame_info.offset & 0xfff) {
        psb__error_message("CI Frame offset must be page aligned!\n");
        /* return  VA_STATUS_ERROR_UNKNOWN; */
    }

    /* all sanity check passed */
    surfaceID = object_heap_allocate( &driver_data->surface_heap );
    obj_surface = SURFACE(surfaceID);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        return vaStatus;
    }
        
    obj_surface->surface_id = surfaceID;
    *surface = surfaceID;
    obj_surface->context_id = -1;
    obj_surface->width = frame_info.width;
    obj_surface->height = frame_info.height;
    obj_surface->subpictures = NULL;
    obj_surface->subpic_count = 0; 
    obj_surface->derived_imgcnt = 0;

    psb_surface = (psb_surface_p) malloc(sizeof(struct psb_surface_s));
    if (NULL == psb_surface)
    {
        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
        obj_surface->surface_id = VA_INVALID_SURFACE;

        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;

        DEBUG_FAILURE;

        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
        obj_surface->surface_id = VA_INVALID_SURFACE;
        
        return vaStatus;
    }

    vaStatus = psb_surface_create_camera( driver_data, frame_info.width, frame_info.height,
                                          frame_info.stride, frame_info.stride * frame_info.height,
                                          psb_surface,
                                          0, /* not V4L2 */
                                          frame_info.offset);
    if ( VA_STATUS_SUCCESS != vaStatus )
    {
        free(psb_surface);
        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
        obj_surface->surface_id = VA_INVALID_SURFACE;

        DEBUG_FAILURE;

        return vaStatus;
    }

    memset(psb_surface->extra_info, 0, sizeof(psb_surface->extra_info));
    psb_surface->extra_info[4] = VA_FOURCC_NV12;

    obj_surface->psb_surface = psb_surface;

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        object_surface_p obj_surface = SURFACE(*surface);
        psb__destroy_surface(driver_data, obj_surface);
        *surface = VA_INVALID_SURFACE;
    }
    
    return vaStatus;
}
    
VAStatus psb_DestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    INIT_DRIVER_DATA
    psb_output_p output = GET_OUTPUT_DATA(ctx);
    int i;

    if (num_surfaces <= 0)
    {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (NULL == surface_list)
    {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    /* Make validation happy */
    for(i = num_surfaces; i--; )
    {
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        if (obj_surface == NULL)
        {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (obj_surface->derived_imgcnt > 0) {
            psb__error_message("Some surface is deriving by images\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    for(i = num_surfaces; i--; )
    {
        object_surface_p obj_surface = SURFACE(surface_list[i]);
        psb__destroy_surface(driver_data, obj_surface);
        surface_list[i] = VA_INVALID_SURFACE;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus psb_CreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context        /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    psb_output_p output = GET_OUTPUT_DATA(ctx);
    object_config_p obj_config;
    int cmdbuf_num, encode=0;
    int i, ret;

    if (num_render_targets <= 0)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    if (NULL == render_targets)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }
    if (NULL == context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }

    vaStatus = psb__checkSurfaceDimensions(driver_data, picture_width, picture_height);
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        DEBUG_FAILURE;
        return vaStatus;
    }

    obj_config = CONFIG(config_id);
    if (NULL == obj_config)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONFIG;
        DEBUG_FAILURE;
        return vaStatus;
    }

    int contextID = object_heap_allocate( &driver_data->context_heap );
    object_context_p obj_context = CONTEXT(contextID);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        return vaStatus;
    }

    *context = contextID;
    obj_context->driver_data = driver_data;
    obj_context->current_render_target = NULL;
    obj_context->is_oold = output->is_oold;

    obj_context->context_id = contextID;
    obj_context->config_id = config_id;
    obj_context->picture_width = picture_width;
    obj_context->picture_height = picture_height;
    obj_context->num_render_targets = num_render_targets;
    obj_context->render_targets = (VASurfaceID *) malloc(num_render_targets * sizeof(VASurfaceID));
    if (obj_context->render_targets == NULL)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;

        object_heap_free( &driver_data->context_heap, (object_base_p) obj_context);
        
        return vaStatus;
    }

    /* allocate buffer points for vaRenderPicture */
    obj_context->num_buffers = 10;
    obj_context->buffer_list = (object_buffer_p *) malloc(sizeof(object_buffer_p) * obj_context->num_buffers);
    if (obj_context->buffer_list == NULL) {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;

        free(obj_context->render_targets);
        object_heap_free( &driver_data->context_heap, (object_base_p) obj_context);
        
        return vaStatus;
    }
    
    memset(obj_context->buffers_unused, 0, sizeof(obj_context->buffers_unused));
    memset(obj_context->buffers_unused_count, 0, sizeof(obj_context->buffers_unused_count));
    memset(obj_context->buffers_unused_tail, 0, sizeof(obj_context->buffers_unused_tail));
    memset(obj_context->buffers_active, 0, sizeof(obj_context->buffers_active));

    if (obj_config->entrypoint == VAEntrypointEncSlice 
	|| obj_config->entrypoint == VAEntrypointEncPicture) {
        encode = 1;
        cmdbuf_num = LNC_MAX_CMDBUFS_ENCODE;
    } else 
        cmdbuf_num = PSB_MAX_CMDBUFS;
    
    for(i = 0; i < num_render_targets; i++)
    {
        object_surface_p obj_surface = SURFACE(render_targets[i]);
        psb_surface_p psb_surface;
        
        if (NULL == obj_surface)
        {
            vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
            DEBUG_FAILURE;
            break;
        }

        psb_surface = obj_surface->psb_surface;
        
        /* Clear format specific surface info */
        obj_context->render_targets[i] = render_targets[i];
        obj_surface->context_id = contextID; /* Claim ownership of surface */

#if 0
        /* for decode, move the surface into |TT */
        if ((encode == 0) && /* decode */
            ((psb_surface->buf.pl_flags & DRM_PSB_FLAG_MEM_RAR) == 0)) /* surface not in RAR */  
            psb_buffer_setstatus(&obj_surface->psb_surface->buf,
                                 WSBM_PL_FLAG_TT | WSBM_PL_FLAG_SHARED, DRM_PSB_FLAG_MEM_MMU);
#endif
    }
    obj_context->va_flags = flag;
    obj_context->format_vtable = obj_config->format_vtable;
    obj_context->format_data = NULL;

    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = obj_context->format_vtable->createContext(obj_context, obj_config);
    }

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        obj_context->context_id = -1;
        obj_context->config_id = -1;
        obj_context->picture_width = 0;
        obj_context->picture_height = 0;
        free(obj_context->render_targets);
        free(obj_context->buffer_list);
        obj_context->num_buffers = 0;
        obj_context->render_targets = NULL;
        obj_context->num_render_targets = 0;
        obj_context->va_flags = 0;
        object_heap_free( &driver_data->context_heap, (object_base_p) obj_context);

        return vaStatus;
    }
    
    /* initialize cmdbuf */
    for (i = 0; i < LNC_MAX_CMDBUFS_ENCODE; i++) {
        obj_context->lnc_cmdbuf_list[i] = NULL;
    }

    for (i = 0; i < PNW_MAX_CMDBUFS_ENCODE; i++) {
        obj_context->pnw_cmdbuf_list[i] = NULL;
    }

    for (i = 0; i < PSB_MAX_CMDBUFS; i++) {
        obj_context->cmdbuf_list[i] = NULL;
    }
    
    for(i = 0; i < cmdbuf_num; i++)
    {
        void  *cmdbuf;
            
        if (encode)  /* Topaz encode context */{
	    if (IS_MFLD(obj_context->driver_data))
		cmdbuf = malloc(sizeof(struct pnw_cmdbuf_s)); 
	    else
		cmdbuf = malloc(sizeof(struct lnc_cmdbuf_s));
	}
        else  /* MSVDX decode context */
            cmdbuf =  malloc(sizeof(struct psb_cmdbuf_s));
            
        if (NULL == cmdbuf)
        {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            break;
        }
            
        if (encode) /* Topaz encode context */{
	    if (IS_MFLD(obj_context->driver_data))
		vaStatus = pnw_cmdbuf_create(obj_context, driver_data, (pnw_cmdbuf_p)cmdbuf);
	    else
		vaStatus = lnc_cmdbuf_create(obj_context, driver_data, (lnc_cmdbuf_p)cmdbuf);
	}
        else  /* MSVDX decode context */
            vaStatus = psb_cmdbuf_create(obj_context, driver_data, (psb_cmdbuf_p)cmdbuf);

        if (VA_STATUS_SUCCESS != vaStatus)
        {
            free(cmdbuf);
            DEBUG_FAILURE;
            break;
        }
        if (encode) {
	   if (IS_MFLD(obj_context->driver_data)) 
            obj_context->pnw_cmdbuf_list[i] = (pnw_cmdbuf_p)cmdbuf;
	   else
            obj_context->lnc_cmdbuf_list[i] = (lnc_cmdbuf_p)cmdbuf;
	}
        else 
            obj_context->cmdbuf_list[i] = (psb_cmdbuf_p)cmdbuf;
    }
    obj_context->cmdbuf_current = -1;
    obj_context->cmdbuf = NULL;
    obj_context->lnc_cmdbuf = NULL;
    obj_context->frame_count = 0;
    obj_context->slice_count = 0;
    obj_context->msvdx_context = driver_data->msvdx_context_base | (contextID & 0xffff);

    obj_context->entry_point = obj_config->entrypoint;
    
    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        if (cmdbuf_num > LNC_MAX_CMDBUFS_ENCODE)
	    cmdbuf_num = LNC_MAX_CMDBUFS_ENCODE;	
        for(i = 0; i < cmdbuf_num; i++)
        {
            if (obj_context->lnc_cmdbuf_list[i])
            {
                lnc_cmdbuf_destroy( obj_context->lnc_cmdbuf_list[i] );
                free(obj_context->lnc_cmdbuf_list[i]);
                obj_context->lnc_cmdbuf_list[i] = NULL;
	    }
            if (obj_context->pnw_cmdbuf_list[i])
            {
                pnw_cmdbuf_destroy( obj_context->pnw_cmdbuf_list[i] );
                free(obj_context->pnw_cmdbuf_list[i]);
                obj_context->pnw_cmdbuf_list[i] = NULL;
            }
            if (obj_context->cmdbuf_list[i])
            {
                psb_cmdbuf_destroy( obj_context->cmdbuf_list[i] );
                free(obj_context->cmdbuf_list[i]);
                obj_context->cmdbuf_list[i] = NULL;
            }
        }
        
        obj_context->cmdbuf = NULL;
        obj_context->lnc_cmdbuf = NULL;

        obj_context->context_id = -1;
        obj_context->config_id = -1;
        obj_context->picture_width = 0;
        obj_context->picture_height = 0;
        free(obj_context->render_targets);
        free(obj_context->buffer_list);
        obj_context->num_buffers = 0;
        obj_context->render_targets = NULL;
        obj_context->num_render_targets = 0;
        obj_context->va_flags = 0;
        object_heap_free( &driver_data->context_heap, (object_base_p) obj_context);
    }
    else
    {
        if (getenv("PSB_VIDEO_NO_OSPM") == NULL) { 
            if (encode)
                ret = lnc_ospm_event_send("video_record", "start");
            else
                ret = lnc_ospm_event_send("video_playback", "start");

            if (ret != 0)
                psb__information_message("lnc_ospm_event_send start error: #%d\n", ret);
            else
                psb__information_message("lnc_ospm_event_send start ok\n");
        }
    }
    return vaStatus;
}

static VAStatus psb__allocate_malloc_buffer(object_buffer_p obj_buffer, int size)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    obj_buffer->buffer_data = realloc(obj_buffer->buffer_data, size);
    if (NULL == obj_buffer->buffer_data)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
    }
    return vaStatus;
}

static VAStatus psb__unmap_buffer( object_buffer_p obj_buffer );

static VAStatus psb__allocate_BO_buffer(psb_driver_data_p driver_data, object_buffer_p obj_buffer, int size, void *data, VABufferType type)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    ASSERT( NULL == obj_buffer->buffer_data );

    if (obj_buffer->psb_buffer && (psb_bs_queued == obj_buffer->psb_buffer->status))
    {
        psb__information_message("Abandoning BO for buffer %08x type %s\n", obj_buffer->base.id,
                                 buffer_type_to_string(obj_buffer->type));
        /* need to set psb_buffer aside and get another one */
        obj_buffer->psb_buffer->status = psb_bs_abandoned;
        obj_buffer->psb_buffer = NULL;
        obj_buffer->size = 0;
        obj_buffer->alloc_size = 0;
    }
 
    if (type == VAProtectedSliceDataBufferType) {
        if (obj_buffer->psb_buffer) {
            psb__information_message("RAR: old RAR slice buffer with RAR handle 0%08x, current RAR handle 0x%08x\n",
                                     obj_buffer->psb_buffer->rar_handle, (uint32_t)data);
            psb__information_message("RAR: force old RAR buffer destroy and new buffer re-allocation by set size=0\n");
            obj_buffer->alloc_size = 0;
        }
    }
    
    if (obj_buffer->alloc_size < size)
    {
        psb__information_message("Buffer size mismatch: Need %d, currently have %d\n", size, obj_buffer->alloc_size);
        if (obj_buffer->psb_buffer)
        {
            if (obj_buffer->buffer_data)
            {
                 psb__unmap_buffer(obj_buffer);
            }
            psb_buffer_destroy( obj_buffer->psb_buffer );
            obj_buffer->alloc_size = 0;
        }
        else
        {
            obj_buffer->psb_buffer = (psb_buffer_p) malloc(sizeof(struct psb_buffer_s));
            if (NULL == obj_buffer->psb_buffer)
            {
                vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
                DEBUG_FAILURE;
            } else
                memset(obj_buffer->psb_buffer, 0, sizeof(struct psb_buffer_s));
        }
        if (VA_STATUS_SUCCESS == vaStatus)
        {
            psb__information_message("Allocate new GPU buffers for vaCreateBuffer:type=%s,size=%d.\n",
                                     buffer_type_to_string(obj_buffer->type), size);
            
            size = (size + 0x7fff) & ~0x7fff; /* Round up */
            if (obj_buffer->type == VAImageBufferType) /* Xserver side PutSurface, Image/subpicture buffer
                                                        * should be shared between two process
                                                        */
                vaStatus = psb_buffer_create( driver_data, size, psb_bt_cpu_vpu_shared, obj_buffer->psb_buffer);
            else if (obj_buffer->type == VAProtectedSliceDataBufferType)
                vaStatus = psb_buffer_reference_rar( driver_data, (uint32_t)data, obj_buffer->psb_buffer );
            else
                vaStatus = psb_buffer_create( driver_data, size, psb_bt_cpu_vpu, obj_buffer->psb_buffer);
            if (VA_STATUS_SUCCESS != vaStatus)
            {
                free(obj_buffer->psb_buffer);
                obj_buffer->psb_buffer = NULL;
                DEBUG_FAILURE;
            }
            else
            {
                obj_buffer->alloc_size = size;
            }
        }
    }
    return vaStatus;
}

static VAStatus psb__map_buffer( object_buffer_p obj_buffer )
{
    if (obj_buffer->psb_buffer)
    {
        return psb_buffer_map( obj_buffer->psb_buffer, &obj_buffer->buffer_data );
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus psb__unmap_buffer( object_buffer_p obj_buffer )
{
    if (obj_buffer->psb_buffer)
    {
        obj_buffer->buffer_data = NULL;
        return psb_buffer_unmap( obj_buffer->psb_buffer );
    }
    return VA_STATUS_SUCCESS;
}

static void psb__destroy_buffer(psb_driver_data_p driver_data, object_buffer_p obj_buffer)
{
    if ( obj_buffer->psb_buffer )
    {
        if (obj_buffer->buffer_data)
        {
            psb__unmap_buffer(obj_buffer);
        }
        psb_buffer_destroy( obj_buffer->psb_buffer );
        free(obj_buffer->psb_buffer);
        obj_buffer->psb_buffer = NULL;
    }

    if (NULL != obj_buffer->buffer_data)
    {
        free(obj_buffer->buffer_data);
        obj_buffer->buffer_data = NULL;
        obj_buffer->size = 0;
    }
    
    object_heap_free( &driver_data->buffer_heap, (object_base_p) obj_buffer);
}

void psb__suspend_buffer(psb_driver_data_p driver_data, object_buffer_p obj_buffer)
{
    if (obj_buffer->context)
    {
        VABufferType type = obj_buffer->type;
        object_context_p obj_context = obj_buffer->context;
    
        /* Remove buffer from active list */
        *obj_buffer->pptr_prev_next = obj_buffer->ptr_next;

        /* Add buffer to tail of unused list */
        obj_buffer->ptr_next = NULL;
        obj_buffer->last_used = obj_context->frame_count;
        if (obj_context->buffers_unused_tail[type])
        {
            obj_buffer->pptr_prev_next = &(obj_context->buffers_unused_tail[type]->ptr_next);
        }
        else
        {
            obj_buffer->pptr_prev_next = &(obj_context->buffers_unused[type]);
        }
        *obj_buffer->pptr_prev_next = obj_buffer;
        obj_context->buffers_unused_tail[type] = obj_buffer;
        obj_context->buffers_unused_count[type]++;
        
        psb__information_message("Adding buffer %08x type %s to unused list. unused count = %d\n", obj_buffer->base.id,
                                 buffer_type_to_string(obj_buffer->type), obj_context->buffers_unused_count[type]);
        
        object_heap_suspend_object((object_base_p) obj_buffer, 1); /* suspend */
        return;
    }
    
    if (obj_buffer->psb_buffer && (psb_bs_queued == obj_buffer->psb_buffer->status))
    {
        /* need to set psb_buffer aside */
        obj_buffer->psb_buffer->status = psb_bs_abandoned;
        obj_buffer->psb_buffer = NULL;
    }

    psb__destroy_buffer(driver_data, obj_buffer);
}

static void psb__destroy_context(psb_driver_data_p driver_data, object_context_p obj_context)
{
    int i;

    if (getenv("PSB_VIDEO_NO_OSPM") == NULL) {
        int encode, ret;
        
        if (obj_context->entry_point == VAEntrypointEncSlice) 
            encode = 1;
        else
            encode = 0;

        if (encode)
            ret = lnc_ospm_event_send("video_record", "stop");
        else
            ret = lnc_ospm_event_send("video_playback", "stop");
        
        if (ret != 0)
            psb__information_message("lnc_ospm_event_send stop error: #%d\n", ret);
        else
            psb__information_message("lnc_ospm_event_send stop ok\n");
    }
    
    
    obj_context->format_vtable->destroyContext(obj_context);

    for(i = 0; i < PSB_MAX_BUFFERTYPES; i++)
    {
        object_buffer_p obj_buffer;
        obj_buffer = obj_context->buffers_active[i];
        for(; obj_buffer; obj_buffer = obj_buffer->ptr_next)
        {
            psb__information_message("%s: destroying active buffer %08x\n", __FUNCTION__, obj_buffer->base.id);
            psb__destroy_buffer(driver_data, obj_buffer);
        }
        obj_buffer = obj_context->buffers_unused[i];
        for(; obj_buffer; obj_buffer = obj_buffer->ptr_next)
        {
            psb__information_message("%s: destroying unused buffer %08x\n", __FUNCTION__, obj_buffer->base.id);
            psb__destroy_buffer(driver_data, obj_buffer);
        }
        obj_context->buffers_unused_count[i] = 0;
    }

    for(i = 0; i < LNC_MAX_CMDBUFS_ENCODE; i++)
    {
        if (obj_context->lnc_cmdbuf_list[i])
        {
            lnc_cmdbuf_destroy( obj_context->lnc_cmdbuf_list[i] );
            free(obj_context->lnc_cmdbuf_list[i]);
            obj_context->lnc_cmdbuf_list[i] = NULL;
        }
    }
    
    for(i = 0; i < PSB_MAX_CMDBUFS; i++)
    {
        if (obj_context->cmdbuf_list[i])
        {
            psb_cmdbuf_destroy( obj_context->cmdbuf_list[i] );
            free(obj_context->cmdbuf_list[i]);
            obj_context->cmdbuf_list[i] = NULL;
        }
    }
    obj_context->cmdbuf = NULL;
    obj_context->lnc_cmdbuf = NULL;

    obj_context->context_id = -1;
    obj_context->config_id = -1;
    obj_context->picture_width = 0;
    obj_context->picture_height = 0;
    if (obj_context->render_targets)
        free(obj_context->render_targets);
    obj_context->render_targets = NULL;
    obj_context->num_render_targets = 0;
    obj_context->va_flags = 0;

    obj_context->current_render_target = NULL;
    if (obj_context->buffer_list)
        free(obj_context->buffer_list);
    obj_context->num_buffers = 0;

    object_heap_free( &driver_data->context_heap, (object_base_p) obj_context);
}

VAStatus psb_DestroyContext(
        VADriverContextP ctx,
        VAContextID context
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context = CONTEXT(context);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }
  
    psb__destroy_context(driver_data, obj_context);    
    
    return vaStatus;
}

VAStatus psb__CreateBuffer(
        psb_driver_data_p driver_data,
        object_context_p obj_context,	/* in */
        VABufferType type,	/* in */
        unsigned int size,    	/* in */
        unsigned int num_elements, /* in */
        void *data,		/* in */
        VABufferID *buf_desc    /* out */
    )
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int bufferID;
    object_buffer_p obj_buffer = obj_context ? obj_context->buffers_unused[type] : NULL;
    int unused_count = obj_context ? obj_context->buffers_unused_count[type] : 0;


    /*
     * Buffer Management
     * For each buffer type, maintain
     *   - a LRU sorted list of unused buffers
     *   - a list of active buffers
     * We only create a new buffer when
     *   - no unused buffers are available
     *   - the last unused buffer is still queued
     *   - the last unused buffer was used very recently and may still be fenced
     *      - used recently is defined as within the current frame_count (subject to tweaks)
     *     
     * The buffer that is returned will be moved to the list of active buffers
     *   - vaDestroyBuffer and vaRenderPicture will move the active buffer back to the list of unused buffers
    */
    psb__information_message("Requesting buffer creation, size=%d,elements=%d,type=%s\n", size,num_elements,
                             buffer_type_to_string(type));

    if ((type == VAProtectedSliceDataBufferType) && (data == NULL)) {
        psb__error_message("RAR: Create protected slice buffer, but RAR handle is NULL\n");
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE ;
    }

    if (obj_buffer && obj_buffer->psb_buffer)
    {
        if (psb_bs_queued == obj_buffer->psb_buffer->status)
        {
            /* Buffer is still queued, allocate new buffer instead */
            psb__information_message("Skipping idle buffer %08x, still queued\n", obj_buffer->base.id);
            obj_buffer = NULL;
        }
        else if ((obj_buffer->last_used == obj_context->frame_count) && (unused_count < MAX_UNUSED_BUFFERS))
        {
            /* Buffer was used for this frame, allocate new buffer instead */
            psb__information_message("Skipping idle buffer %08x, recently used. Unused = %d\n", obj_buffer->base.id, unused_count);
            obj_buffer = NULL;
        }
    }
    
    if (obj_buffer)
    {
        bufferID = obj_buffer->base.id;
        psb__information_message("Reusing buffer %08x type %s from unused list. Unused = %d\n", bufferID,
                                 buffer_type_to_string(type), unused_count);

        /* Remove from unused list */
        obj_context->buffers_unused[type] = obj_buffer->ptr_next;
        if (obj_context->buffers_unused[type])
        {
            obj_context->buffers_unused[type]->pptr_prev_next = &(obj_context->buffers_unused[type]);
            ASSERT(obj_context->buffers_unused_tail[type] != obj_buffer);
        }
        else
        {
            ASSERT(obj_context->buffers_unused_tail[type] == obj_buffer);
            obj_context->buffers_unused_tail[type] = 0;
        }
        obj_context->buffers_unused_count[type]--;

        object_heap_suspend_object((object_base_p)obj_buffer, 0); /* Make BufferID valid again */
        ASSERT(type == obj_buffer->type);
        ASSERT(obj_context == obj_buffer->context);
    }
    else
    {
        bufferID = object_heap_allocate( &driver_data->buffer_heap );
        obj_buffer = BUFFER(bufferID);
        if (NULL == obj_buffer)
        {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            DEBUG_FAILURE;
            return vaStatus;
        }
        psb__information_message("Allocating new buffer %08x type %s.\n", bufferID, buffer_type_to_string(type));
        obj_buffer->type = type;
        obj_buffer->buffer_data = NULL;
        obj_buffer->psb_buffer = NULL;
        obj_buffer->size = 0;
        obj_buffer->max_num_elements = 0;
        obj_buffer->alloc_size = 0;
        obj_buffer->context = obj_context;
    }
    if (obj_context)
    {
        /* Add to front of active list */
        obj_buffer->ptr_next = obj_context->buffers_active[type];
        if (obj_buffer->ptr_next)
        {
            obj_buffer->ptr_next->pptr_prev_next = &(obj_buffer->ptr_next);
        }
        obj_buffer->pptr_prev_next = &(obj_context->buffers_active[type]);
        *obj_buffer->pptr_prev_next = obj_buffer;
    }

    switch (obj_buffer->type)
    {
      case VABitPlaneBufferType:
      case VASliceDataBufferType:
      case VAResidualDataBufferType:
      case VAImageBufferType:
      case VASliceGroupMapBufferType: 
      case VAEncCodedBufferType:
      case VAProtectedSliceDataBufferType:
            vaStatus = psb__allocate_BO_buffer(driver_data, obj_buffer, size * num_elements, data, obj_buffer->type);
            DEBUG_FAILURE;
            break;
      case VAPictureParameterBufferType:
      case VAIQMatrixBufferType:
      case VASliceParameterBufferType:
      case VAMacroblockParameterBufferType:
      case VADeblockingParameterBufferType:
      case VAEncSequenceParameterBufferType:
      case VAEncPictureParameterBufferType:
      case VAEncSliceParameterBufferType:
      case VAQMatrixBufferType:
            psb__information_message("Allocate new malloc buffers for vaCreateBuffer:type=%s,size=%d, buffer_data=%p.\n",
                                     buffer_type_to_string(type), size, obj_buffer->buffer_data);
            vaStatus = psb__allocate_malloc_buffer(obj_buffer, size * num_elements);
            DEBUG_FAILURE;
            break;

      default:
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            DEBUG_FAILURE;
            break;;
    }

    if (VA_STATUS_SUCCESS == vaStatus)
    {
        obj_buffer->size = size;
        obj_buffer->max_num_elements = num_elements;
        obj_buffer->num_elements = num_elements;
        if (data && (obj_buffer->type != VAProtectedSliceDataBufferType))
        {
            vaStatus = psb__map_buffer(obj_buffer);
            if (VA_STATUS_SUCCESS == vaStatus)
            {
                memcpy(obj_buffer->buffer_data, data, size * num_elements);

                psb__unmap_buffer(obj_buffer);
            }
        }
    }
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        *buf_desc = bufferID;
    }
    else
    {
        psb__destroy_buffer(driver_data, obj_buffer);
    }

    return vaStatus;
}

VAStatus psb_CreateBuffer(
        VADriverContextP ctx,
        VAContextID context,	/* in */
        VABufferType type,	/* in */
        unsigned int size,    	/* in */
        unsigned int num_elements, /* in */
        void *data,		/* in */
        VABufferID *buf_desc    /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    if (num_elements <= 0)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    switch (type)
    {
      case VABitPlaneBufferType:
      case VASliceDataBufferType:
      case VAProtectedSliceDataBufferType:
      case VAResidualDataBufferType:
      case VASliceGroupMapBufferType:
      case VAPictureParameterBufferType:
      case VAIQMatrixBufferType:
      case VASliceParameterBufferType:
      case VAMacroblockParameterBufferType:
      case VADeblockingParameterBufferType:
      case VAEncCodedBufferType:
      case VAEncSequenceParameterBufferType:
      case VAEncPictureParameterBufferType:
      case VAEncSliceParameterBufferType:
      case VAQMatrixBufferType:
            break;

      default:
            vaStatus = VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            DEBUG_FAILURE;
            return vaStatus;
    }
    
    object_context_p obj_context = CONTEXT(context);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }
    if (NULL == buf_desc)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    return psb__CreateBuffer(driver_data, obj_context, type, size, num_elements, data, buf_desc);
}


VAStatus psb_BufferInfo(
        VADriverContextP ctx,
        VAContextID context,	/* in */
        VABufferID buf_id,	/* in */
        VABufferType *type,	/* out */
        unsigned int *size,    	/* out */
        unsigned int *num_elements /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;

    object_buffer_p obj_buffer = BUFFER(buf_id);
    if (NULL == obj_buffer)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    *type = obj_buffer->type;
    *size = obj_buffer->size;
    *num_elements = obj_buffer->num_elements;
    return VA_STATUS_SUCCESS;
}


VAStatus psb_BufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,    /* in */
        unsigned int num_elements    /* in */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    if (NULL == obj_buffer)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    if ((num_elements <= 0) || (num_elements > obj_buffer->max_num_elements))
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        obj_buffer->num_elements = num_elements;
    }

    return vaStatus;
}

VAStatus psb_MapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,    /* in */
        void **pbuf         /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    if (NULL == obj_buffer)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }


    if (NULL == pbuf)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    vaStatus = psb__map_buffer(obj_buffer);
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    if (NULL != obj_buffer->buffer_data)
    {
        *pbuf = obj_buffer->buffer_data;

        /* specifically for Topaz encode
         * write validate coded data offset in CodedBuffer
         */
        if (obj_buffer->type == VAEncCodedBufferType)
            psb_codedbuf_map_mangle(ctx, obj_buffer, pbuf);
            /* *(IMG_UINT32 *)((void *)obj_buffer->buffer_data + 4) = 16; */
    }
    else
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    return vaStatus;
}

VAStatus psb_UnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id    /* in */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buf_id);
    if (NULL == obj_buffer)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    vaStatus = psb__unmap_buffer(obj_buffer);

    return vaStatus;
}


VAStatus psb_DestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_buffer_p obj_buffer = BUFFER(buffer_id);
    
    if (NULL == obj_buffer)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    psb__suspend_buffer(driver_data, obj_buffer);
    return vaStatus;
}
    

VAStatus psb_BeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context;
    object_surface_p obj_surface;
    object_config_p obj_config;
    
    obj_context = CONTEXT(context);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }

    /* Must not be within BeginPicture / EndPicture already */
    ASSERT(obj_context->current_render_target == NULL);

    obj_surface = SURFACE(render_target);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    obj_context->current_render_target = obj_surface;
    obj_context->slice_count = 0;

    obj_config = CONFIG(obj_context->config_id);
    /* if the surface is decode render target, and in displaying */
    if (obj_config &&
        (obj_config->entrypoint != VAEntrypointEncSlice) &&
        (driver_data->cur_displaying_surface == render_target))
        psb__error_message("WARNING: rendering a displaying surface, may see tearing\n");
    
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = obj_context->format_vtable->beginPicture(obj_context);
    }

    psb__information_message("---BeginPicture 0x%08x for frame %d --\n",
                             render_target, obj_context->frame_count);
#ifdef DEBUG_TRACE
    psb__trace_message("------Trace frame %d------\n", obj_context->frame_count);
#endif
    
    return vaStatus;
}

VAStatus psb_RenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_context_p obj_context;
    object_buffer_p *buffer_list;
    int i;
    
    obj_context = CONTEXT(context);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    if (num_buffers <= 0)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    
    if (NULL == buffers)
    {
        /* Don't crash on NULL pointers */
        vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
        DEBUG_FAILURE;
        return vaStatus;
    }
    /* Must be within BeginPicture / EndPicture */
    ASSERT(obj_context->current_render_target != NULL);

    if (num_buffers > obj_context->num_buffers) {
        free(obj_context->buffer_list);
        
        obj_context->buffer_list = (object_buffer_p *) malloc(sizeof(object_buffer_p) * num_buffers);
        if (obj_context->buffer_list ==NULL)
        {
            vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
            obj_context->num_buffers = 0;
        }

        obj_context->num_buffers = num_buffers;
    }
    buffer_list = obj_context->buffer_list;
    
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        /* Lookup buffer references */
        for(i = 0; i < num_buffers; i++)
        {
            object_buffer_p obj_buffer = BUFFER(buffers[i]);
            if (NULL == obj_buffer)
            {
                vaStatus = VA_STATUS_ERROR_INVALID_BUFFER;
                DEBUG_FAILURE;
            }
            buffer_list[i] = obj_buffer;
        }
    }

    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = obj_context->format_vtable->renderPicture(obj_context, buffer_list, num_buffers);
    }

    if (buffer_list)
    {
        /* Release buffers */
        for(i = 0; i < num_buffers; i++)
        {
            if (buffer_list[i])
            {
                 psb__suspend_buffer(driver_data, buffer_list[i]);
            }
        }
    }

    return vaStatus;
}

VAStatus psb_EndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus;
    object_context_p obj_context;
 
    obj_context = CONTEXT(context);
    if (NULL == obj_context)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_CONTEXT;
        DEBUG_FAILURE;
        return vaStatus;
    }

    vaStatus = obj_context->format_vtable->endPicture(obj_context);

    psb__information_message("---EndPicture for frame %d --\n", obj_context->frame_count);
    
    obj_context->current_render_target = NULL;
    obj_context->frame_count++;
#ifdef DEBUG_TRACE
    psb__trace_message("FrameCount = %03d\n", obj_context->frame_count);
    psb__information_message("FrameCount = %03d\n", obj_context->frame_count);
    psb__trace_message(NULL);
#endif

    return vaStatus;
}


static unsigned long GetTickCount()
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
        return 0;
    return tv.tv_usec/1000+tv.tv_sec*1000;
}

VAStatus psb_SyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_surface_p obj_surface;
    
    obj_surface = SURFACE(render_target);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    /* The cur_displaying_surface indicates the surface being displayed by overlay.
     * The diaplay_timestamp records the time point of put surface, which would
     * be set to zero while using texture blit.*/

    /* don't use mutex here for performance concern... */
    //pthread_mutex_lock(&output->output_mutex);
    if ( render_target == driver_data->cur_displaying_surface )
        vaStatus = VA_STATUS_ERROR_SURFACE_IN_DISPLAYING;
    else if ( ( VA_INVALID_SURFACE != driver_data->cur_displaying_surface ) /* use overlay */ 
            && (render_target == driver_data->last_displaying_surface ))   /* It's the last displaying surface*/ 
    {
        object_surface_p cur_obj_surface = SURFACE(driver_data->cur_displaying_surface);
        /*  The flip operation on current displaying surface could be delayed to
         *  next VBlank and hadn't been finished yet. Then, the last displaying
         *  surface shouldn't be freed, because the hardware may not 
         *  complete loading data of it. Any change of the last surface could 
         *  have a impect on the scrren.*/
        if (NULL != cur_obj_surface) {
            while ((GetTickCount() - cur_obj_surface->display_timestamp) < PSB_MAX_FLIP_DELAY) 
                usleep(PSB_MAX_FLIP_DELAY * 1000);
        }
    }
    //pthread_mutex_unlock(&output->output_mutex);

    if (vaStatus != VA_STATUS_ERROR_SURFACE_IN_DISPLAYING)
        vaStatus = psb_surface_sync(obj_surface->psb_surface);
    
    DEBUG_FAILURE;
    return vaStatus;
}


VAStatus psb_QuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status    /* out */
    )
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    object_surface_p obj_surface;
    object_context_p obj_context;
    object_config_p obj_config;
    context_ENC_p ctx_enc;
    VASurfaceStatus surface_status;
    int frame_skip = 0;
    psb_output_p output = GET_OUTPUT_DATA(ctx);
    
    obj_surface = SURFACE(render_target);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }
    if (NULL == status)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        DEBUG_FAILURE;
        return vaStatus;
    }

    vaStatus = psb_surface_query_status(obj_surface->psb_surface, &surface_status);

    /* The cur_displaying_surface indicates the surface being displayed by overlay.
     * The diaplay_timestamp records the time point of put surface, which would
     * be set to zero while using texture blit.*/
    pthread_mutex_lock(&output->output_mutex);
    if ( render_target == driver_data->cur_displaying_surface )
        surface_status = VASurfaceDisplaying; 
    else if ( ( VA_INVALID_SURFACE != driver_data->cur_displaying_surface ) /* use overlay */ 
            && (render_target == driver_data->last_displaying_surface ))   /* It's the last displaying surface*/ 
    {
        object_surface_p cur_obj_surface = SURFACE(driver_data->cur_displaying_surface);
        /*The flip operation on current displaying surface could be delayed to
         *  next VBlank and hadn't been finished yet. Then, the last displaying
         *  surface shouldn't be freed, because the hardware may not 
         *  complete loading data of it. Any change of the last surface could 
         *  have a impect on the scrren.*/
        if ( (NULL != cur_obj_surface)
                && ((GetTickCount() - cur_obj_surface->display_timestamp) < PSB_MAX_FLIP_DELAY))
        {
            surface_status = VASurfaceDisplaying;
        }
    }
    pthread_mutex_unlock(&output->output_mutex);

    /* try to get frameskip flag */    
    obj_context = CONTEXT(obj_surface->context_id);    
    if (NULL == obj_context) /* not associate with a context */
        goto out_done;
        
    obj_config = CONFIG(obj_context->config_id);
    if (NULL == obj_config) /* not have a validate context */
        goto out_done;

    if (obj_config->entrypoint != VAEntrypointEncSlice)
        goto out_done; /* not encode context */
        
    ctx_enc = (context_ENC_p) obj_context->format_data;
    if (ctx_enc->sRCParams.RCEnable == 0)
        goto out_done; /* the context is not in RC mode */

    lnc_surface_get_frameskip(obj_context->driver_data, obj_surface->psb_surface, &frame_skip);
    if (frame_skip == 1)
        surface_status = surface_status | VASurfaceSkipped;

out_done:
    *status = surface_status;
    
    return vaStatus;
}

VAStatus psb_LockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* following are output argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name,
        void **buffer 
)
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    void *surface_data;
    int ret;
    
    object_surface_p obj_surface = SURFACE(surface);
    psb_surface_p psb_surface;
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    psb_surface = obj_surface->psb_surface;
    if (buffer_name) {
	/* todo */
    }

    if (buffer) { /* map the surface buffer */
	uint32_t srf_buf_ofs = 0;
	ret = psb_buffer_map(&psb_surface->buf, &surface_data);
	if (ret)
	{
	    *buffer = NULL;
	    vaStatus = VA_STATUS_ERROR_UNKNOWN;
	    DEBUG_FAILURE;
	    return vaStatus;
	}
	srf_buf_ofs = psb_surface->buf.buffer_ofs;
	*buffer = surface_data + srf_buf_ofs;
    }

    if (buffer) { /* map the surface buffer */
        uint32_t srf_buf_ofs = 0;
        ret = psb_buffer_map(&psb_surface->buf, &surface_data);
        if (ret)
        {
            *buffer = NULL;
            vaStatus = VA_STATUS_ERROR_UNKNOWN;
            DEBUG_FAILURE;
            return vaStatus;
        }
        srf_buf_ofs = psb_surface->buf.buffer_ofs;
        *buffer = surface_data + srf_buf_ofs;
    }

    *fourcc = VA_FOURCC_NV12;
    *luma_stride = psb_surface->stride;
    *chroma_u_stride = psb_surface->stride;
    *chroma_v_stride = psb_surface->stride;
    *luma_offset = 0;
    *chroma_u_offset = obj_surface->height * psb_surface->stride;
    *chroma_v_offset = obj_surface->height * psb_surface->stride + 1;

    return vaStatus;
}


VAStatus psb_UnlockSurface(
        VADriverContextP ctx,
        VASurfaceID surface
)
{
    INIT_DRIVER_DATA
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    
    object_surface_p obj_surface = SURFACE(surface);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_INVALID_SURFACE;
        DEBUG_FAILURE;
        return vaStatus;
    }

    psb_surface_p psb_surface = obj_surface->psb_surface;
    
    psb_buffer_unmap(&psb_surface->buf);
    
    return VA_STATUS_SUCCESS;
}

VAStatus psb_CreateSurfaceFromV4L2Buf(
    VADriverContextP ctx,
    int v4l2_fd,         /* file descriptor of V4L2 device */
    struct v4l2_format *v4l2_fmt,       /* format of V4L2 */
    struct v4l2_buffer *v4l2_buf,       /* V4L2 buffer */
    VASurfaceID *surface	/* out */
    )
{
    INIT_DRIVER_DATA;
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    
    int surfaceID;
    object_surface_p obj_surface;
    psb_surface_p psb_surface;
    int width, height, buf_stride, buf_offset, size;
    
    if (IS_MRST(driver_data) == 0) {
        psb__error_message("CreateSurfaceFromV4L2Buf isn't supported on non-MRST platform\n");
        return VA_STATUS_ERROR_UNKNOWN;
    }
    
    /* Todo:
     * sanity check if the v4l2 device on MRST is supported
     */
    
    surfaceID = object_heap_allocate( &driver_data->surface_heap );
    obj_surface = SURFACE(surfaceID);
    if (NULL == obj_surface)
    {
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE;
        return vaStatus;
    }

    width = v4l2_fmt->fmt.pix.width;
    height = v4l2_fmt->fmt.pix.height;

    buf_stride = width; /* ? */
    buf_offset = v4l2_buf->m.offset;
    size = v4l2_buf->length;

    psb__information_message("Create Surface from V4L2 buffer: %dx%d, stride=%d, buffer offset=0x%08x, size=%d\n",
                             width, height, buf_stride, buf_offset, size);
    
    obj_surface->surface_id = surfaceID;
    *surface = surfaceID;
    obj_surface->context_id = -1;
    obj_surface->width = width;
    obj_surface->height = height;
    obj_surface->subpictures = NULL;
    obj_surface->subpic_count = 0; 
    obj_surface->derived_imgcnt = 0;
    obj_surface->display_timestamp = 0;

    psb_surface = (psb_surface_p) malloc(sizeof(struct psb_surface_s));
    if (NULL == psb_surface)
    {
        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
        obj_surface->surface_id = VA_INVALID_SURFACE;

        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;

        DEBUG_FAILURE;

        return vaStatus;
    }

    /* current assume it is NV12 */
    vaStatus = psb_surface_create_camera( driver_data, width, height, buf_stride, size, psb_surface, 1, buf_offset);
    if ( VA_STATUS_SUCCESS != vaStatus )
    {
        free(psb_surface);
        object_heap_free( &driver_data->surface_heap, (object_base_p) obj_surface);
        obj_surface->surface_id = VA_INVALID_SURFACE;

        DEBUG_FAILURE;

        return vaStatus;
    }

    memset(psb_surface->extra_info, 0, sizeof(psb_surface->extra_info));
    psb_surface->extra_info[4] = VA_FOURCC_NV12; /* temp treat is as IYUV */

    obj_surface->psb_surface = psb_surface;

    /* Error recovery */
    if (VA_STATUS_SUCCESS != vaStatus)
    {
        object_surface_p obj_surface = SURFACE(*surface);
        psb__destroy_surface(driver_data, obj_surface);
        *surface = VA_INVALID_SURFACE;
    }
    
    return vaStatus;
}


int  LOCK_HARDWARE(psb_driver_data_p driver_data)
{                                                                       
    char ret=0;
    
    if (driver_data->dri2 || driver_data->dri_dummy)
        return 0;                                

    pthread_mutex_lock(&driver_data->drm_mutex);                    
    DRM_CAS(driver_data->drm_lock, driver_data->drm_context,        
            (DRM_LOCK_HELD|driver_data->drm_context), ret);        
    if (ret) {                                                     
        ret = drmGetLock(driver_data->drm_fd, driver_data->drm_context, 0); 
        /* driver_data->contended_lock=1; */                        
    }                                                               

    return ret;
}

int UNLOCK_HARDWARE(psb_driver_data_p driver_data)                                  
{                                                                
    /* driver_data->contended_lock=0; */                            
    if (driver_data->dri2 || driver_data->dri_dummy)
        return 0;
    
    DRM_UNLOCK(driver_data->drm_fd,driver_data->drm_lock,driver_data->drm_context); 
    pthread_mutex_unlock(&driver_data->drm_mutex);

    return 0;
}


static void psb__deinitDRM( VADriverContextP ctx )
{
	INIT_DRIVER_DATA

        if (driver_data->main_pool) {
            driver_data->main_pool->takeDown(driver_data->main_pool);
            driver_data->main_pool = NULL;
	}
	if (driver_data->fence_mgr) {
		wsbmFenceMgrTTMTakedown(driver_data->fence_mgr);
		driver_data->fence_mgr = NULL;
	}

	if (wsbmIsInitialized())
		wsbmTakedown();
	    
	driver_data->psb_sarea = NULL;
	driver_data->drm_fd  = -1;
	driver_data->sarea_handle = 0;
}


static VAStatus psb__initDRI( VADriverContextP ctx )
{
    INIT_DRIVER_DATA
    struct dri_state *dri_state = (struct dri_state *)ctx->dri_state;

    assert(dri_state);
    assert(dri_state->driConnectedFlag == VA_DRI2 || 
           dri_state->driConnectedFlag == VA_DRI1 ||
	   dri_state->driConnectedFlag == VA_DUMMY);

    driver_data->drm_fd = dri_state->fd;
    driver_data->dri_dummy = (dri_state->driConnectedFlag == VA_DUMMY);
    driver_data->dri2 = (dri_state->driConnectedFlag == VA_DRI2);
    driver_data->sarea_handle = 0;
    driver_data->sarea_map = NULL;
    driver_data->psb_sarea = NULL;
    driver_data->dri_priv = NULL;
    driver_data->bus_id = NULL;
#ifndef ANDROID
    if (!driver_data->dri2 && !driver_data->dri_dummy) {
        drm_sarea_t *pSAREA;

        pSAREA = (drm_sarea_t *)dri_state->pSAREA;
        driver_data->drm_lock = (drmLock *)(&pSAREA->lock);
        driver_data->psb_sarea = (void *)pSAREA + sizeof(drm_sarea_t);
	driver_data->drm_context = dri_state->hwContext;
    }
#endif 
    return VA_STATUS_SUCCESS;
}


static VAStatus psb__initTTM( VADriverContextP ctx )
{
    INIT_DRIVER_DATA
    
    const char drm_ext[] = "psb_ttm_placement_alphadrop";
    union drm_psb_extension_arg arg;
    struct _WsbmBufferPool *pool;
    int ret;
    const char exec_ext[] = "psb_ttm_execbuf_alphadrop";
    union drm_psb_extension_arg exec_arg;
    const char lncvideo_getparam_ext[] = "lnc_video_getparam";
    union drm_psb_extension_arg lncvideo_getparam_arg;

    /* init wsbm */
    ret = wsbmInit(wsbmNullThreadFuncs(), psbVNodeFuncs());
    if (ret) {
	    psb__error_message("failed initializing libwsbm.\n");
	    return VA_STATUS_ERROR_UNKNOWN;
    }
    
    strncpy(arg.extension, drm_ext, sizeof(arg.extension));
    /* FIXME: should check dri enabled?
     * it seems not init dri here at all
     */
    ret = drmCommandWriteRead(driver_data->drm_fd, DRM_PSB_EXTENSION,
			    &arg, sizeof(arg));
    if (ret != 0 || !arg.rep.exists) {
	    psb__error_message("failed to detect DRM extension \"%s\".\n",
			    drm_ext);
	    driver_data->main_pool = NULL;
	    return VA_STATUS_ERROR_UNKNOWN;
    } else {
	    pool = wsbmTTMPoolInit(driver_data->drm_fd,
			    arg.rep.driver_ioctl_offset);
	    if (pool == NULL) {
		    psb__error_message("failed to get ttm pool\n");
		    return VA_STATUS_ERROR_UNKNOWN;
	    }
	    driver_data->main_pool = pool;
    }

    strncpy(exec_arg.extension, exec_ext, sizeof(exec_arg.extension));
    ret = drmCommandWriteRead(driver_data->drm_fd, DRM_PSB_EXTENSION, &exec_arg, 
			      sizeof(exec_arg));
    if (ret != 0 || !exec_arg.rep.exists) {
        psb__error_message("failed to detect DRM extension \"%s\".\n",
                            exec_ext);
	return FALSE;
    }
    driver_data->execIoctlOffset = exec_arg.rep.driver_ioctl_offset;

    strncpy(lncvideo_getparam_arg.extension, lncvideo_getparam_ext, sizeof(lncvideo_getparam_arg.extension));
    ret = drmCommandWriteRead(driver_data->drm_fd, DRM_PSB_EXTENSION, &lncvideo_getparam_arg,
			      sizeof(lncvideo_getparam_arg));
    if (ret != 0 || !lncvideo_getparam_arg.rep.exists) {
	    psb__error_message("failed to detect DRM extension \"%s\".\n",
			       lncvideo_getparam_ext);
	    /* return FALSE; */ /* not reture FALSE, so it still can run */
    }
    driver_data->getParamIoctlOffset = lncvideo_getparam_arg.rep.driver_ioctl_offset;
    return VA_STATUS_SUCCESS;
}

static VAStatus psb__initDRM( VADriverContextP ctx )
{
    VAStatus vaStatus;

    vaStatus = psb__initDRI(ctx);

    if (vaStatus == VA_STATUS_SUCCESS)
        return psb__initTTM(ctx);
    else
        return vaStatus;
}

VAStatus psb_Terminate( VADriverContextP ctx )
{
    INIT_DRIVER_DATA
    object_subpic_p obj_subpic;
    object_image_p obj_image;
    object_buffer_p obj_buffer;
    object_surface_p obj_surface;
    object_context_p obj_context;
    object_config_p obj_config;
    object_heap_iterator iter;

    /* Clean up left over contexts */
    obj_context = (object_context_p) object_heap_first( &driver_data->context_heap, &iter);
    while (obj_context)
    {
        psb__information_message("vaTerminate: contextID %08x still allocated, destroying\n", obj_context->base.id);
        psb__destroy_context(driver_data, obj_context);
        obj_context = (object_context_p) object_heap_next( &driver_data->context_heap, &iter);
    }
    object_heap_destroy( &driver_data->context_heap );

    /* Clean up SubpicIDs */
    obj_subpic = (object_subpic_p) object_heap_first( &driver_data->subpic_heap, &iter);
    while (obj_subpic)
    {
        psb__information_message("vaTerminate: subpictureID %08x still allocated, destroying\n", obj_subpic->base.id);
        psb__destroy_subpicture(driver_data, obj_subpic);
        obj_subpic = (object_subpic_p) object_heap_next( &driver_data->subpic_heap, &iter);
    }
    object_heap_destroy( &driver_data->subpic_heap );
    
    /* Clean up ImageIDs */
    obj_image = (object_image_p) object_heap_first( &driver_data->image_heap, &iter);
    while (obj_image)
    {
        psb__information_message("vaTerminate: imageID %08x still allocated, destroying\n", obj_image->base.id);
        psb__destroy_image(driver_data, obj_image);
        obj_image = (object_image_p) object_heap_next( &driver_data->image_heap, &iter);
    }
    object_heap_destroy( &driver_data->image_heap );

    /* Clean up left over buffers */
    obj_buffer = (object_buffer_p) object_heap_first( &driver_data->buffer_heap, &iter);
    while (obj_buffer)
    {
        psb__information_message("vaTerminate: bufferID %08x still allocated, destroying\n", obj_buffer->base.id);
        psb__destroy_buffer(driver_data, obj_buffer);
        obj_buffer = (object_buffer_p) object_heap_next( &driver_data->buffer_heap, &iter);
    }
    object_heap_destroy( &driver_data->buffer_heap );

    /* Clean up left over surfaces */
    obj_surface = (object_surface_p) object_heap_first( &driver_data->surface_heap, &iter);
    while (obj_surface)
    {
        psb__information_message("vaTerminate: surfaceID %08x still allocated, destroying\n", obj_surface->base.id);
        psb__destroy_surface(driver_data, obj_surface);
        obj_surface = (object_surface_p) object_heap_next( &driver_data->surface_heap, &iter);
    }
    object_heap_destroy( &driver_data->surface_heap );

    /* Clean up configIDs */
    obj_config = (object_config_p) object_heap_first( &driver_data->config_heap, &iter);
    while (obj_config)
    {
        object_heap_free( &driver_data->config_heap, (object_base_p) obj_config);
        obj_config = (object_config_p) object_heap_next( &driver_data->config_heap, &iter);
    }
    object_heap_destroy( &driver_data->config_heap );


    if (driver_data->camera_bo) {
        psb_buffer_destroy((psb_buffer_p)driver_data->camera_bo);
        free(driver_data->camera_bo);
        driver_data->camera_bo = NULL;
    }

    if (driver_data->rar_bo) {
        psb_buffer_destroy((psb_buffer_p)driver_data->rar_bo);
        free(driver_data->rar_bo);
        driver_data->rar_bo = NULL;
    }

    if (driver_data->rar_rd) {
        RAR_desc_t *rar_rd = driver_data->rar_rd;

        RAR_fini(rar_rd);
        free(driver_data->rar_rd);
        driver_data->rar_rd = NULL;
    }
    
    if (driver_data->video_output) {
        psb_deinitOutput(ctx);
        driver_data->video_output = NULL;
    }

    psb__deinitDRM(ctx);
    free(ctx->pDriverData);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

EXPORT VAStatus __vaDriverInit_0_31(  VADriverContextP ctx )
{
    psb_driver_data_p driver_data;
    struct VADriverVTableTPI *tpi;
    int result;

#ifdef DEBUG_TRACE
    /* make gdb always stop here */
    signal(SIGUSR1,SIG_IGN);
    kill(getpid(),SIGUSR1);
#endif
    
    ctx->version_major = 0;
    ctx->version_minor = 31;
    
    ctx->max_profiles = PSB_MAX_PROFILES;
    ctx->max_entrypoints = PSB_MAX_ENTRYPOINTS;
    ctx->max_attributes = PSB_MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = PSB_MAX_IMAGE_FORMATS;
    ctx->max_subpic_formats = PSB_MAX_SUBPIC_FORMATS;
    ctx->max_display_attributes = PSB_MAX_DISPLAY_ATTRIBUTES;
    ctx->str_vendor = PSB_STR_VENDOR;
    
    ctx->vtable.vaTerminate = psb_Terminate;
    ctx->vtable.vaQueryConfigEntrypoints = psb_QueryConfigEntrypoints;
    ctx->vtable.vaTerminate = psb_Terminate;
    ctx->vtable.vaQueryConfigProfiles = psb_QueryConfigProfiles;
    ctx->vtable.vaQueryConfigEntrypoints = psb_QueryConfigEntrypoints;
    ctx->vtable.vaQueryConfigAttributes = psb_QueryConfigAttributes;
    ctx->vtable.vaCreateConfig = psb_CreateConfig;
    ctx->vtable.vaDestroyConfig = psb_DestroyConfig;
    ctx->vtable.vaGetConfigAttributes = psb_GetConfigAttributes;
    ctx->vtable.vaCreateSurfaces = psb_CreateSurfaces;
    ctx->vtable.vaDestroySurfaces = psb_DestroySurfaces;
    ctx->vtable.vaCreateContext = psb_CreateContext;
    ctx->vtable.vaDestroyContext = psb_DestroyContext;
    ctx->vtable.vaCreateBuffer = psb_CreateBuffer;
    ctx->vtable.vaBufferSetNumElements = psb_BufferSetNumElements;
    ctx->vtable.vaMapBuffer = psb_MapBuffer;
    ctx->vtable.vaUnmapBuffer = psb_UnmapBuffer;
    ctx->vtable.vaDestroyBuffer = psb_DestroyBuffer;
    ctx->vtable.vaBeginPicture = psb_BeginPicture;
    ctx->vtable.vaRenderPicture = psb_RenderPicture;
    ctx->vtable.vaEndPicture = psb_EndPicture;
    ctx->vtable.vaSyncSurface = psb_SyncSurface;
    ctx->vtable.vaQuerySurfaceStatus = psb_QuerySurfaceStatus;
    ctx->vtable.vaPutSurface = psb_PutSurface;
    ctx->vtable.vaQueryImageFormats = psb_QueryImageFormats;
    ctx->vtable.vaCreateImage = psb_CreateImage;
    ctx->vtable.vaDeriveImage = psb_DeriveImage;
    ctx->vtable.vaDestroyImage = psb_DestroyImage;
    ctx->vtable.vaSetImagePalette = psb_SetImagePalette;
    ctx->vtable.vaGetImage = psb_GetImage;
    ctx->vtable.vaPutImage = psb_PutImage;
    ctx->vtable.vaQuerySubpictureFormats = psb_QuerySubpictureFormats;
    ctx->vtable.vaCreateSubpicture = psb_CreateSubpicture;
    ctx->vtable.vaDestroySubpicture = psb_DestroySubpicture;
    ctx->vtable.vaSetSubpictureImage = psb_SetSubpictureImage;
    ctx->vtable.vaSetSubpictureChromakey = psb_SetSubpictureChromakey;
    ctx->vtable.vaSetSubpictureGlobalAlpha = psb_SetSubpictureGlobalAlpha;
    ctx->vtable.vaAssociateSubpicture = psb_AssociateSubpicture;
    ctx->vtable.vaDeassociateSubpicture = psb_DeassociateSubpicture;
    ctx->vtable.vaQueryDisplayAttributes = psb_QueryDisplayAttributes;
    ctx->vtable.vaGetDisplayAttributes = psb_GetDisplayAttributes;
    ctx->vtable.vaSetDisplayAttributes = psb_SetDisplayAttributes;
    ctx->vtable.vaBufferInfo = psb_BufferInfo;
    ctx->vtable.vaLockSurface = psb_LockSurface;
    ctx->vtable.vaUnlockSurface = psb_UnlockSurface;
    ctx->vtable_tpi = malloc(sizeof(struct VADriverVTableTPI));
    if (NULL == ctx->vtable_tpi)
	return VA_STATUS_ERROR_ALLOCATION_FAILED;

    tpi = (struct VADriverVTableTPI *)ctx->vtable_tpi;
    tpi->vaCreateSurfaceFromCIFrame = psb_CreateSurfaceFromCIFrame;
    tpi->vaCreateSurfaceFromV4L2Buf = psb_CreateSurfaceFromV4L2Buf;
#ifdef ANDROID    
    tpi->vaPutSurfaceBuf = psb_PutSurfaceBuf;
#endif
    driver_data = (psb_driver_data_p) malloc( sizeof(*driver_data) );
    ctx->pDriverData = (void *) driver_data;
    if ( NULL == driver_data)
    {
        if (ctx->vtable_tpi)
            free(ctx->vtable_tpi);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    memset(driver_data,0, sizeof(*driver_data));/* clear it */

    if (VA_STATUS_SUCCESS != psb__initDRM(ctx))
    {
        free(ctx->pDriverData);
        ctx->pDriverData = NULL;
        return VA_STATUS_ERROR_UNKNOWN;
    }

    pthread_mutex_init(&driver_data->drm_mutex, NULL);

/*
 * To read PBO.MSR.CCF Mode and Status Register C-Spec -p112
 */
#define PCI_PORT5_REG80_VIDEO_SD_DISABLE	0x0008
#define PCI_PORT5_REG80_VIDEO_HD_DISABLE	0x0010

#if 0
    struct drm_psb_hw_info hw_info;
    do {
        result = drmCommandRead(driver_data->drm_fd, DRM_PSB_HW_INFO, &hw_info, sizeof(hw_info));
    } while (result == EAGAIN);

    if (result != 0)
    {
        psb__deinitDRM(ctx);
        free(ctx->pDriverData);
        ctx->pDriverData = NULL;
        return VA_STATUS_ERROR_UNKNOWN;
    }
    
    driver_data->video_sd_disabled = !!(hw_info.caps & PCI_PORT5_REG80_VIDEO_SD_DISABLE);
    driver_data->video_hd_disabled = !!(hw_info.caps & PCI_PORT5_REG80_VIDEO_HD_DISABLE);
    psb__information_message("hw_info: rev_id = %08x capabilities = %08x\n", hw_info.rev_id, hw_info.caps);
    psb__information_message("hw_info: video_sd_disable=%d,video_hd_disable=%d\n",
                             driver_data->video_sd_disabled,driver_data->video_hd_disabled);
    if (driver_data->video_sd_disabled != 0) {
        psb__error_message("MRST: hw_info shows video_sd_disable is true,fix it manually\n");
        driver_data->video_sd_disabled = 0;
    }
    if (driver_data->video_hd_disabled != 0) {
        psb__error_message("MRST: hw_info shows video_hd_disable is true,fix it manually\n");
        driver_data->video_hd_disabled = 0;
    }
#endif

    if (0 != psb_get_device_info(ctx)) {
        psb__error_message("ERROR: failed to get video device info\n");
        driver_data->encode_supported = 1;
        driver_data->decode_supported = 1;
        driver_data->hd_encode_supported = 1;
        driver_data->hd_decode_supported = 1;
    }	    

    struct dri_state *dri_state = (struct dri_state *)ctx->dri_state;
    if (dri_state->driConnectedFlag == VA_DRI1 ||
        dri_state->driConnectedFlag == VA_DRI2 ||
	dri_state->driConnectedFlag == VA_DUMMY) {
        if (VA_STATUS_SUCCESS != psb_initOutput(ctx)) {
            psb__deinitDRM(ctx);
            free(ctx->pDriverData);
            ctx->pDriverData = NULL;
            return VA_STATUS_ERROR_UNKNOWN;
        }
    }
    
    driver_data->msvdx_context_base = (((unsigned int) getpid()) & 0xffff) << 16;

    //    driver_data->profile2Format[VAProfileMPEG2Simple] = &psb_MPEG2_vtable;
    driver_data->profile2Format[VAProfileMPEG2Main][VAEntrypointVLD] = &psb_MPEG2_vtable;
    driver_data->profile2Format[VAProfileMPEG2Main][VAEntrypointMoComp] = &psb_MPEG2MC_vtable;

    driver_data->profile2Format[VAProfileMPEG4Simple][VAEntrypointVLD] = &psb_MPEG4_vtable;
    driver_data->profile2Format[VAProfileMPEG4AdvancedSimple][VAEntrypointVLD] = &psb_MPEG4_vtable;
    //    driver_data->profile2Format[VAProfileMPEG4Main][VAEntrypointVLD] = &psb_MPEG4_vtable;

    driver_data->profile2Format[VAProfileH264Baseline][VAEntrypointVLD] = &psb_H264_vtable;
    driver_data->profile2Format[VAProfileH264Main][VAEntrypointVLD] = &psb_H264_vtable;
    driver_data->profile2Format[VAProfileH264High][VAEntrypointVLD] = &psb_H264_vtable;

    driver_data->profile2Format[VAProfileVC1Simple][VAEntrypointVLD] = &psb_VC1_vtable;
    driver_data->profile2Format[VAProfileVC1Main][VAEntrypointVLD] = &psb_VC1_vtable;
    driver_data->profile2Format[VAProfileVC1Advanced][VAEntrypointVLD] = &psb_VC1_vtable;

    if (IS_MFLD(driver_data))
    {
	driver_data->profile2Format[VAProfileH263Baseline][VAEntrypointEncSlice] = &pnw_H263ES_vtable;
        driver_data->profile2Format[VAProfileH264Baseline][VAEntrypointEncSlice] = &pnw_H264ES_vtable;
        driver_data->profile2Format[VAProfileH264Main][VAEntrypointEncSlice] = &pnw_H264ES_vtable;
        driver_data->profile2Format[VAProfileMPEG4Simple][VAEntrypointEncSlice] = &pnw_MPEG4ES_vtable;
        driver_data->profile2Format[VAProfileMPEG4AdvancedSimple][VAEntrypointEncSlice] = &pnw_MPEG4ES_vtable;
        driver_data->profile2Format[VAProfileJPEGBaseline][VAEntrypointEncPicture] = &pnw_JPEG_vtable;
/*
        driver_data->profile2Format[VAProfileMPEG2Main][VAEntrypointVLD] = &pnw_MPEG2_vtable;

        driver_data->profile2Format[VAProfileMPEG4Simple][VAEntrypointVLD] = &pnw_MPEG4_vtable;
        driver_data->profile2Format[VAProfileMPEG4AdvancedSimple][VAEntrypointVLD] = &pnw_MPEG4_vtable;

        driver_data->profile2Format[VAProfileH264Baseline][VAEntrypointVLD] = &pnw_H264_vtable;
        driver_data->profile2Format[VAProfileH264Main][VAEntrypointVLD] = &pnw_H264_vtable;
        driver_data->profile2Format[VAProfileH264High][VAEntrypointVLD] = &pnw_H264_vtable;

        driver_data->profile2Format[VAProfileVC1Simple][VAEntrypointVLD] = &pnw_VC1_vtable;
        driver_data->profile2Format[VAProfileVC1Main][VAEntrypointVLD] = &pnw_VC1_vtable;
        driver_data->profile2Format[VAProfileVC1Advanced][VAEntrypointVLD] = &pnw_VC1_vtable;
*/
    }
    else if (IS_MRST(driver_data) && driver_data->encode_supported) {
        driver_data->profile2Format[VAProfileH263Baseline][VAEntrypointEncSlice] = &lnc_H263ES_vtable;
        driver_data->profile2Format[VAProfileH264Baseline][VAEntrypointEncSlice] = &lnc_H264ES_vtable;
        driver_data->profile2Format[VAProfileH264Main][VAEntrypointEncSlice] = &lnc_H264ES_vtable;
        driver_data->profile2Format[VAProfileMPEG4Simple][VAEntrypointEncSlice] = &lnc_MPEG4ES_vtable;
        driver_data->profile2Format[VAProfileMPEG4AdvancedSimple][VAEntrypointEncSlice] = &lnc_MPEG4ES_vtable;
    }
    
    result = object_heap_init( &driver_data->config_heap, sizeof(struct object_config_s), CONFIG_ID_OFFSET );
    ASSERT( result == 0 );

    result = object_heap_init( &driver_data->context_heap, sizeof(struct object_context_s), CONTEXT_ID_OFFSET );
    ASSERT( result == 0 );

    result = object_heap_init( &driver_data->surface_heap, sizeof(struct object_surface_s), SURFACE_ID_OFFSET );
    ASSERT( result == 0 );

    result = object_heap_init( &driver_data->buffer_heap, sizeof(struct object_buffer_s), BUFFER_ID_OFFSET );
    ASSERT( result == 0 );

    result = object_heap_init( &driver_data->image_heap, sizeof(struct object_image_s), IMAGE_ID_OFFSET );
    ASSERT( result == 0 );

    result = object_heap_init( &driver_data->subpic_heap, sizeof(struct object_subpic_s), SUBPIC_ID_OFFSET );
    ASSERT( result == 0 );

    driver_data->cur_displaying_surface = VA_INVALID_SURFACE;
    driver_data->last_displaying_surface = VA_INVALID_SURFACE;
    return VA_STATUS_SUCCESS;
}

static int psb_get_device_info( VADriverContextP ctx )
{
    INIT_DRIVER_DATA;
    struct drm_lnc_video_getparam_arg arg;
    unsigned long device_info;
    int ret = 0;
    unsigned long video_capability;
    unsigned long pci_device;

    driver_data->dev_id = 0x4100; /* by default MRST */
    
    arg.key = LNC_VIDEO_DEVICE_INFO;
    arg.value = (uint64_t)((unsigned long) &device_info);
    ret = drmCommandWriteRead(driver_data->drm_fd, driver_data->getParamIoctlOffset,
                              &arg, sizeof(arg));
    if (ret==0) {
        pci_device = (device_info >> 16) & 0xffff;
        video_capability = device_info & 0xffff;

        driver_data->dev_id = pci_device;
        psb__information_message("Retrieve Device ID 0x%04x\n", driver_data->dev_id);
        
        if ((IS_MRST(driver_data) && (pci_device != 0x4101)) ||
            IS_MFLD(driver_data))
	    driver_data->encode_supported = 1;
        else /* 0x4101 or other device hasn't encode support */
	    driver_data->encode_supported = 0;

        driver_data->decode_supported = !(video_capability & 0x2);
        driver_data->hd_decode_supported = !(video_capability & 0x3);
        driver_data->hd_encode_supported = !(video_capability & 0x4);

        psb__information_message("video capability: decode %s, HD decode %s\n",
                                 driver_data->decode_supported? "support": "not support",
                                 driver_data->hd_decode_supported? "support": "not support");

        psb__information_message("video capability: encode %s, HD encode %s\n",
                                 driver_data->encode_supported? "support": "not support",
                                 driver_data->hd_encode_supported? "support": "not support");

        
        return ret;
    }
    
    psb__information_message("failed to get video device info\n");

    return ret;  
}
