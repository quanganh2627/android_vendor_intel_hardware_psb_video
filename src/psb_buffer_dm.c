/*
 * INTEL CONFIDENTIAL
 * Copyright 2007 Intel Corporation. All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its suppliers
 * or licensors. Title to the Material remains with Intel Corporation or its
 * suppliers and licensors. The Material may contain trade secrets and
 * proprietary and confidential information of Intel Corporation and its
 * suppliers and licensors, and is protected by worldwide copyright and trade
 * secret laws and treaty provisions. No part of the Material may be used,
 * copied, reproduced, modified, published, uploaded, posted, transmitted,
 * distributed, or disclosed in any way without Intel's prior express written
 * permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 */

#include "psb_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <wsbm/wsbm_manager.h>
/* #include <linux/rar_register.h> */

#include "psb_drm.h"
#include "psb_def.h"


static VAStatus psb_buffer_offset_camerav4l2(psb_driver_data_p driver_data,
        psb_buffer_p buf,
        unsigned int v4l2_buf_offset,
        unsigned int *bo_offset
                                            )
{
    *bo_offset = v4l2_buf_offset;
    return VA_STATUS_SUCCESS;
}


static VAStatus psb_buffer_offset_cameraci(psb_driver_data_p driver_data,
        psb_buffer_p buf,
        unsigned int ci_frame_offset_or_handle,
        unsigned int *bo_offset
                                          )
{
    *bo_offset = ci_frame_offset_or_handle;

    return VA_STATUS_SUCCESS;
}


static int psb_buffer_info_ci(psb_driver_data_p driver_data)
{
    struct drm_lnc_video_getparam_arg arg;
    unsigned long camera_info[2] = {0, 0};
    int ret = 0;

    driver_data->camera_phyaddr = driver_data->camera_size = 0;

    arg.key = LNC_VIDEO_GETPARAM_CI_INFO;
    arg.value = (uint64_t)((unsigned long) & camera_info[0]);
    ret = drmCommandWriteRead(driver_data->drm_fd, driver_data->getParamIoctlOffset,
                              &arg, sizeof(arg));
    if (ret == 0) {
        driver_data->camera_phyaddr = camera_info[0];
        driver_data->camera_size = camera_info[1];
        psb__information_message("CI region physical address = 0x%08x, size=%dK\n",
                                 driver_data->camera_phyaddr,  driver_data->camera_size / 1024);

        return ret;
    }

    psb__information_message("CI region get_info failed\n");
    return ret;
}

/*
 * Allocate global BO which maps camear device memory as encode MMU memory
 * the global BO shared by several encode surfaces created from camear memory
 */
static VAStatus psb_buffer_init_camera(psb_driver_data_p driver_data)
{
    int ret = 0;

    /* hasn't grab camera device memory region
     * grab the whole 4M camera device memory
     */
    driver_data->camera_bo = calloc(1, sizeof(struct psb_buffer_s));
    if (driver_data->camera_bo == NULL)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    psb__information_message("Grab whole camera device memory\n");
    ret = psb_buffer_create(driver_data, driver_data->camera_size, psb_bt_camera, (psb_buffer_p) driver_data->camera_bo);

    if (ret != VA_STATUS_SUCCESS) {
        free(driver_data->camera_bo);
        driver_data->camera_bo = NULL;

        psb__error_message("Grab camera device memory failed\n");
    }

    return ret;
}


/*
 * Create one buffer from camera device memory
 * is_v4l2 means if the buffer is V4L2 buffer
 * id_or_ofs is CI frame ID (actually now is frame offset), or V4L2 buffer offset
 */
VAStatus psb_buffer_create_camera(psb_driver_data_p driver_data,
                                  psb_buffer_p buf,
                                  int is_v4l2,
                                  int id_or_ofs
                                 )
{
    VAStatus vaStatus;
    int ret = 0;
    unsigned int camera_offset = 0;

    if (driver_data->camera_bo  == NULL) {
        if (psb_buffer_info_ci(driver_data)) {
            psb__error_message("Can't get CI region information\n");
            return VA_STATUS_ERROR_UNKNOWN;
        }

        vaStatus = psb_buffer_init_camera(driver_data);
        if (vaStatus != VA_STATUS_SUCCESS) {
            psb__error_message("Grab camera device memory failed\n");
            return ret;
        }
    }

    /* reference the global camear BO */
    ret = psb_buffer_reference(driver_data, buf, (psb_buffer_p) driver_data->camera_bo);
    if (ret != VA_STATUS_SUCCESS) {
        psb__error_message("Reference camera device memory failed\n");
        return ret;
    }

    if (is_v4l2)
        ret = psb_buffer_offset_camerav4l2(driver_data, buf, id_or_ofs, &camera_offset);
    else
        ret = psb_buffer_offset_cameraci(driver_data, buf, id_or_ofs, &camera_offset);

    buf->buffer_ofs = camera_offset;

    return ret;
}

/*
 * Create one buffer from user buffer
 * id_or_ofs is CI frame ID (actually now is frame offset), or V4L2 buffer offset
 * user_ptr :virtual address of user buffer start.
 */
VAStatus psb_buffer_create_camera_from_ub(psb_driver_data_p driver_data,
        psb_buffer_p buf,
        int id_or_ofs,
        int size,
        const unsigned long * user_ptr)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    int allignment;
    uint32_t placement;
    int ret;

    buf->rar_handle = 0;
    buf->buffer_ofs = 0;
    buf->type = psb_bt_user_buffer;
    buf->user_ptr = (void *)user_ptr;
    buf->driver_data = driver_data;

    allignment = 4096;
    placement =  DRM_PSB_FLAG_MEM_MMU | WSBM_PL_FLAG_TT | WSBM_PL_FLAG_CACHED | WSBM_PL_FLAG_SHARED ;
    ret = LOCK_HARDWARE(driver_data);
    if (ret) {
        UNLOCK_HARDWARE(driver_data);
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
        DEBUG_FAILURE_RET;
        return vaStatus;
    }
    ret = wsbmGenBuffers(driver_data->main_pool, 1, &buf->drm_buf,
                         allignment, placement);
    if (!buf->drm_buf) {
        psb__error_message("failed to gen wsbm buffers\n");
        UNLOCK_HARDWARE(driver_data);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

#ifndef ANDROID
    extern int wsbmBODataUB(struct _WsbmBufferObject *buf,
                                unsigned size, const void *data,
                                struct _WsbmBufferPool *newPool, uint32_t placement, const unsigned long *user_ptr);

    /* here use the placement when gen buffer setted */
    ret = wsbmBODataUB(buf->drm_buf, size, NULL, NULL, 0, user_ptr);
    UNLOCK_HARDWARE(driver_data);
    if (ret) {
        psb__error_message("failed to alloc wsbm buffers\n");
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    psb__information_message("Create BO from user buffer 0x%08x (%d byte),BO GPU offset hint=0x%08x\n",
                             user_ptr, size, wsbmBOOffsetHint(buf->drm_buf));

#endif

    buf->pl_flags = placement;
    buf->status = psb_bs_ready;
    buf->wsbm_synccpu_flag = 0;

    return VA_STATUS_SUCCESS;
}


static int psb_buffer_info_rar(psb_driver_data_p driver_data)
{
    struct drm_lnc_video_getparam_arg arg;
    unsigned long rar_info[2] = {0, 0};
    int ret = 0;

    driver_data->rar_phyaddr = driver_data->rar_size = 0;

    arg.key = LNC_VIDEO_GETPARAM_RAR_INFO;
    arg.value = (uint64_t)((unsigned long) & rar_info[0]);
    ret = drmCommandWriteRead(driver_data->drm_fd, driver_data->getParamIoctlOffset,
                              &arg, sizeof(arg));
    if (ret == 0) {
        driver_data->rar_phyaddr = rar_info[0];
        driver_data->rar_size = rar_info[1];
        psb__information_message("RAR region physical address = 0x%08x, size=%dK\n",
                                 driver_data->rar_phyaddr,  driver_data->rar_size / 1024);

        return ret;
    }

    psb__information_message("RAR region get size failed\n");
    return ret;
}


static int psb_buffer_offset_rar(psb_driver_data_p driver_data,
                                 psb_buffer_p buf,
                                 uint32_t rar_handle,
                                 unsigned int *bo_offset
                                )
{
    struct drm_lnc_video_getparam_arg arg;
    unsigned long offset;
    int ret = 0;

    *bo_offset = 0;

    arg.key = LNC_VIDEO_GETPARAM_RAR_HANDLER_OFFSET;
    arg.arg = (uint64_t)((unsigned long) & rar_handle);
    arg.value = (uint64_t)((unsigned long) & offset);
    ret = drmCommandWriteRead(driver_data->drm_fd, driver_data->getParamIoctlOffset,
                              &arg, sizeof(arg));
    if (ret == 0) {
        *bo_offset = offset;

        return ret;
    }

    psb__information_message("RAR buffer 0x%08x, get offset failed\n", rar_handle);
    return ret;
}


static VAStatus psb_buffer_init_rar(psb_driver_data_p driver_data)
{
    int ret = 0;
    RAR_desc_t *rar_rd;

    /* hasn't grab RAR device memory region
     * grab the whole 8M RAR device memory
     */
    driver_data->rar_bo = calloc(1, sizeof(struct psb_buffer_s));
    if (driver_data->rar_bo == NULL)
        goto exit_error;

    driver_data->rar_rd = calloc(1, sizeof(RAR_desc_t));
    if (driver_data->rar_rd == NULL)
        goto exit_error;

    memset(driver_data->rar_rd, 0, sizeof(RAR_desc_t));

    psb__information_message("Init RAR device\n");

    ret = RAR_init(driver_data->rar_rd);
    if (ret != 0) {
        psb__error_message("RAR device init failed\n");
        goto exit_error;
    }

    if (psb_buffer_info_rar(driver_data)) {
        psb__error_message("Get RAR region size failed\n");
        goto exit_error;
    }

    psb__information_message("Grab whole camera device memory\n");
    ret = psb_buffer_create(driver_data, driver_data->rar_size, psb_bt_rar, (psb_buffer_p) driver_data->rar_bo);

    if (ret != VA_STATUS_SUCCESS) {
        psb__error_message("Grab RAR device memory failed\n");
        goto exit_error;
    }

    return VA_STATUS_SUCCESS;

exit_error:
    rar_rd = driver_data->rar_rd;

    if (rar_rd) {
        if (rar_rd->mrfd)
            RAR_fini(driver_data->rar_rd);
        free(rar_rd);
    }

    if (driver_data->rar_bo)
        free(driver_data->rar_bo);

    driver_data->rar_bo = NULL;
    driver_data->rar_rd = NULL;

    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}



/*
 * Create RAR buffer
 * Only used when create a protected surface
 */
VAStatus psb_buffer_create_rar(psb_driver_data_p driver_data,
                               unsigned int size,
                               psb_buffer_p buf
                              )
{
    VAStatus vaStatus;
    uint32_t rar_handle = 0;
    unsigned int rar_offset = 0;
    RAR_desc_t *rar_rd;
    int ret;

    if (driver_data->rar_rd  == NULL) {
        vaStatus = psb_buffer_init_rar(driver_data);
        if (vaStatus != VA_STATUS_SUCCESS) {
            psb__error_message("RAR init failed!\n");
            return vaStatus;
        }
    }

    rar_rd = driver_data->rar_rd;

    /* Call RAR interface to allocate RAR buffers */
    ret = RAR_reserve(rar_rd, size, RAR_TYPE_VIDEO, &rar_handle);
    if (ret != 0) {
        psb__error_message("RAR reserver memory failed\n");
        RAR_fini(rar_rd);

        return VA_STATUS_ERROR_UNKNOWN;
    }

    ret = psb_buffer_offset_rar(driver_data, buf, rar_handle, &rar_offset);
    if (ret != 0) {
        psb__error_message("Get buffer offset of RAR device memory failed!\n");
        return ret;
    }

    /* reference the global RAR BO */
    ret = psb_buffer_reference(driver_data, buf, (psb_buffer_p) driver_data->rar_bo);
    if (ret != VA_STATUS_SUCCESS) {
        psb__error_message("Reference RAR device memory failed\n");
        return ret;
    }

    buf->rar_handle = rar_handle;
    buf->buffer_ofs = rar_offset;

    /* reference the global RAR buffer, reset buffer type */
    buf->type = psb_bt_rar_surface; /* need RAR_release */

    psb__information_message("Create RAR buffer, handle 0x%08x, RAR region offset =0x%08x, RAR BO GPU offset hint=0x%08x\n",
                             rar_handle, rar_offset, wsbmBOOffsetHint(buf->drm_buf));

    return VA_STATUS_SUCCESS;
}


/*
 * Destroy RAR buffer
 */
VAStatus psb_buffer_destroy_rar(psb_driver_data_p driver_data,
                                psb_buffer_p buf
                               )
{
    RAR_desc_t *rar_rd;
    int ret;

    ASSERT(driver_data->rar_rd);

    if (buf->type == psb_bt_rar_slice) {
        psb__information_message("return RAR slice buffer to application\n");
        buf->rar_handle = 0;
        return VA_STATUS_SUCCESS;
    }


    rar_rd = driver_data->rar_rd;

    ret = RAR_release(rar_rd, buf->rar_handle);
    if (ret != 0)
        psb__error_message("RAR release memory failed\n");

    buf->rar_handle = 0;

    return VA_STATUS_SUCCESS;
}

/*
 * Reference one RAR buffer from handle
 * only used to reference a slice RAR buffer which is created outside of video driver
 */
VAStatus psb_buffer_reference_rar(psb_driver_data_p driver_data,
                                  uint32_t rar_handle,
                                  psb_buffer_p buf
                                 )
{
    VAStatus vaStatus;
    unsigned int rar_offset = 0;
    int ret;

    if (driver_data->rar_rd  == NULL) {
        vaStatus = psb_buffer_init_rar(driver_data);
        if (vaStatus != VA_STATUS_SUCCESS) {
            psb__error_message("RAR init failed!\n");
            return vaStatus;
        }
    }

    /* don't need to assign the handle to buffer
     * so that when destroy the buffer, we just
     * need to unreference
     */
    /* buf->rar_handle = rar_handle; */

    ret = psb_buffer_offset_rar(driver_data, buf, rar_handle, &rar_offset);
    if (ret != VA_STATUS_SUCCESS) {
        psb__error_message("Get surfae offset of RAR device memory failed!\n");
        return ret;
    }

    /* reference the global RAR BO */
    ret = psb_buffer_reference(driver_data, buf, (psb_buffer_p) driver_data->rar_bo);
    if (ret != VA_STATUS_SUCCESS) {
        psb__error_message("Reference RAR device memory failed\n");
        return ret;
    }

    buf->rar_handle = rar_handle;
    buf->buffer_ofs = rar_offset;
    /* reference the global RAR buffer, reset buffer type */
    buf->type = psb_bt_rar_slice; /* don't need to RAR_release */

    psb__information_message("Reference RAR buffer, handle 0x%08x, RAR region offset =0x%08x, RAR BO GPU offset hint=0x%08x\n",
                             rar_handle, rar_offset, wsbmBOOffsetHint(buf->drm_buf));

    return VA_STATUS_SUCCESS;
}
