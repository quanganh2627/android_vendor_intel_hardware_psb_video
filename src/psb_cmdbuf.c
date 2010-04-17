/*
 * Copyright (c) 2007 Intel Corporation. All Rights Reserved.
 * Copyright (c) Imagination Technologies Limited, UK 
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


#include "psb_cmdbuf.h"

#include <unistd.h>
#include <stdio.h>

#include "hwdefs/mem_io.h"
#include "hwdefs/msvdx_offsets.h"
#include "hwdefs/dma_api.h"
#include "hwdefs/reg_io2.h"
#include "hwdefs/msvdx_vec_reg_io2.h"
#include "hwdefs/msvdx_vdmc_reg_io2.h"
#include "hwdefs/msvdx_mtx_reg_io2.h"
#include "hwdefs/msvdx_dmac_linked_list.h"
#include "hwdefs/msvdx_rendec_mtx_slice_cntrl_reg_io2.h"
#include "hwdefs/dxva_cmdseq_msg.h"
#include "hwdefs/dxva_fw_ctrl.h"
#include "hwdefs/fwrk_msg_mem_io.h"
#include "hwdefs/dxva_msg.h"
#include "hwdefs/msvdx_cmds_io2.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "psb_def.h"
#include "psb_ws_driver.h"

#include <wsbm/wsbm_pool.h>
#include <wsbm/wsbm_manager.h>
#include <wsbm/wsbm_util.h>
#include <wsbm/wsbm_fencemgr.h>


/*
 * Buffer layout:
 *         cmd_base <= cmd_idx < CMD_END() == lldma_base
 *         lldma_base <= lldma_idx < LLDMA_END() == (cmd_base + size)
 *
 * Reloc buffer layout:
 *         MTX_msg < reloc_base == MTX_msg + MTXMSG_SIZE
 *         reloc_base <= reloc_idx < RELOC_END() == (MTX_msg + reloc_size)
 */
#define MTXMSG_END(cmdbuf)    (cmdbuf->reloc_base)
#define RELOC_END(cmdbuf)     (cmdbuf->MTX_msg + cmdbuf->reloc_size)

#define CMD_END(cmdbuf)       (cmdbuf->lldma_base)
#define LLDMA_END(cmdbuf)     (cmdbuf->cmd_base + cmdbuf->size)

#define MTXMSG_SIZE           (0x1000)
#define RELOC_SIZE            (0x3000)

#define CMD_SIZE              (0x3000)
#define LLDMA_SIZE            (0x2000)

#define MTXMSG_MARGIN         (0x0040)
#define RELOC_MARGIN          (0x0800)

#define CMD_MARGIN            (0x0400)
#define LLDMA_MARGIN          (0x0400)


#define MAX_CMD_COUNT         12

#define MTX_SEG_SIZE          (0x0800)

#define PSB_TIMEOUT_USEC 990000

static void psb_cmdbuf_lldma_create_internal( psb_cmdbuf_p cmdbuf,
                 LLDMA_CMD *pLLDMACmd,
                                 psb_buffer_p bitstream_buf,
                                 uint32_t buffer_offset,
                                 uint32_t size,
                                 uint32_t dest_offset,
                                 LLDMA_TYPE cmd);

/*
 * Create command buffer
 */
VAStatus psb_cmdbuf_create(object_context_p obj_context, psb_driver_data_p driver_data,
        psb_cmdbuf_p cmdbuf
   )
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    unsigned int size = CMD_SIZE + LLDMA_SIZE;
    unsigned int reloc_size = MTXMSG_SIZE + RELOC_SIZE;
    unsigned int regio_size = (obj_context->picture_width >> 4) * (obj_context->picture_height >> 4) * 128;

    cmdbuf->size = 0;
    cmdbuf->reloc_size = 0;
    cmdbuf->regio_size = 0;
    cmdbuf->MTX_msg = NULL;
    cmdbuf->cmd_base = NULL;
    cmdbuf->regio_base = NULL;
    cmdbuf->cmd_idx = NULL;
    cmdbuf->regio_idx = NULL;
    cmdbuf->cmd_bitstream_size = NULL;
    cmdbuf->lldma_base = NULL;
    cmdbuf->lldma_idx = NULL;
    cmdbuf->reloc_base = NULL;
    cmdbuf->reloc_idx = NULL;
    cmdbuf->reg_start = NULL;
    cmdbuf->rendec_block_start = NULL;
    cmdbuf->rendec_chunk_start = NULL;
    cmdbuf->skip_block_start = NULL;
    cmdbuf->last_next_segment_cmd = NULL;
    cmdbuf->buffer_refs_count = 0;
    cmdbuf->buffer_refs_allocated = 10;
    cmdbuf->buffer_refs = (psb_buffer_p *) malloc(sizeof(psb_buffer_p) * cmdbuf->buffer_refs_allocated);
    if (NULL == cmdbuf->buffer_refs)
    {
        cmdbuf->buffer_refs_allocated = 0;
        vaStatus = VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = psb_buffer_create( driver_data, size, psb_bt_cpu_vpu, &cmdbuf->buf );
        cmdbuf->size = size;
    }
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = psb_buffer_create( driver_data, reloc_size, psb_bt_cpu_only, &cmdbuf->reloc_buf );
        cmdbuf->reloc_size = reloc_size;
    }
    if (VA_STATUS_SUCCESS == vaStatus)
    {
        vaStatus = psb_buffer_create( driver_data, regio_size, psb_bt_cpu_only, &cmdbuf->regio_buf );
        cmdbuf->regio_size = regio_size;
    }

    if (VA_STATUS_SUCCESS != vaStatus)
    {
        psb_cmdbuf_destroy(cmdbuf);
    }
    return vaStatus;
}

/*
 * Destroy buffer
 */   
void psb_cmdbuf_destroy( psb_cmdbuf_p cmdbuf )
{
    if (cmdbuf->size)
    {
        psb_buffer_destroy( &cmdbuf->buf );
        cmdbuf->size = 0;
    }
    if (cmdbuf->reloc_size)
    {
        psb_buffer_destroy( &cmdbuf->reloc_buf );
        cmdbuf->reloc_size = 0;
    }
    if (cmdbuf->regio_size)
    {
        psb_buffer_destroy( &cmdbuf->regio_buf );
        cmdbuf->regio_size = 0;
    }
    if (cmdbuf->buffer_refs_allocated)
    {
        free( cmdbuf->buffer_refs );
        cmdbuf->buffer_refs = NULL;
        cmdbuf->buffer_refs_allocated = 0;
    }
}

/*
 * Reset buffer & map
 *
 * Returns 0 on success
 */
int psb_cmdbuf_reset( psb_cmdbuf_p cmdbuf )
{
    int ret;
    
    cmdbuf->MTX_msg = NULL;
    cmdbuf->cmd_base = NULL;
    cmdbuf->cmd_idx = NULL;
    cmdbuf->cmd_bitstream_size = NULL;
    cmdbuf->lldma_base = NULL;
    cmdbuf->lldma_idx = NULL;
    cmdbuf->reloc_base = NULL;
    cmdbuf->reloc_idx = NULL;
    cmdbuf->last_next_segment_cmd = NULL;

    cmdbuf->buffer_refs_count = 0;
    cmdbuf->cmd_count = 0;
    cmdbuf->deblock_count = 0;

    ret = psb_buffer_map( &cmdbuf->buf, &cmdbuf->cmd_base );
    if (ret)
    {
        return ret;
    }
    ret = psb_buffer_map( &cmdbuf->reloc_buf, &cmdbuf->MTX_msg );
    if (ret)
    {
        psb_buffer_unmap( &cmdbuf->buf );
        return ret;
    }
    
    cmdbuf->cmd_start = cmdbuf->cmd_base;
    cmdbuf->cmd_idx = (uint32_t *) cmdbuf->cmd_base;
    cmdbuf->cmd_bitstream_size = NULL;
    cmdbuf->lldma_base = cmdbuf->cmd_base + CMD_SIZE;
    cmdbuf->lldma_idx = cmdbuf->lldma_base;

    cmdbuf->reloc_base = cmdbuf->MTX_msg + MTXMSG_SIZE;
    cmdbuf->reloc_idx = (struct drm_psb_reloc *) cmdbuf->reloc_base;

    /* Add ourselves to the buffer list */
    psb_cmdbuf_buffer_ref(cmdbuf, &cmdbuf->reloc_buf); /* reloc buf == 0 */
    psb_cmdbuf_buffer_ref(cmdbuf, &cmdbuf->buf); /* cmd buf == 1 */
    return ret;
}

/*
 * Unmap buffer
 *
 * Returns 0 on success
 */
int psb_cmdbuf_unmap( psb_cmdbuf_p cmdbuf )
{
    cmdbuf->MTX_msg = NULL;
    cmdbuf->cmd_base = NULL;
    cmdbuf->cmd_start = NULL;
    cmdbuf->cmd_idx = NULL;
    cmdbuf->cmd_bitstream_size = NULL;
    cmdbuf->lldma_base = NULL;
    cmdbuf->lldma_idx = NULL;
    cmdbuf->reloc_base = NULL;
    cmdbuf->reloc_idx = NULL;
    cmdbuf->cmd_count = 0;
    psb_buffer_unmap( &cmdbuf->buf );
    psb_buffer_unmap( &cmdbuf->reloc_buf );
    return 0;
}


/*
 * Reference an addtional buffer "buf" in the command stream
 * Returns a reference index that can be used to refer to "buf" in
 * relocation records, -1 on error
 */
int psb_cmdbuf_buffer_ref( psb_cmdbuf_p cmdbuf, psb_buffer_p buf )
{
	int item_loc = 0;

        // buf->next = NULL; /* buf->next only used for buffer list validation */
        
        while( (item_loc < cmdbuf->buffer_refs_count)
                && (wsbmKBufHandle(wsbmKBuf(cmdbuf->buffer_refs[item_loc]->drm_buf))
                    != wsbmKBufHandle(wsbmKBuf(buf->drm_buf))))
	{
		item_loc++;
	}
	if (item_loc == cmdbuf->buffer_refs_count)
	{
		/* Add new entry */
		if (item_loc >= cmdbuf->buffer_refs_allocated)
		{
			/* Allocate more entries */
			int new_size = cmdbuf->buffer_refs_allocated+10;
			psb_buffer_p *new_array;
			new_array = (psb_buffer_p *) malloc(sizeof(psb_buffer_p) * new_size);
			if (NULL == new_array)
			{
				return -1; /* Allocation failure */
			}
			memcpy(new_array, cmdbuf->buffer_refs, sizeof(psb_buffer_p) * cmdbuf->buffer_refs_allocated);
			free(cmdbuf->buffer_refs);
			cmdbuf->buffer_refs_allocated = new_size;
			cmdbuf->buffer_refs = new_array;
		}
		cmdbuf->buffer_refs[item_loc] = buf;
		cmdbuf->buffer_refs_count++;
		buf->status = psb_bs_queued;
                
                buf->next = NULL;
	}

        /* only for RAR buffers */
        if ((cmdbuf->buffer_refs[item_loc] != buf)
            && (buf->rar_handle != 0)) {
            psb_buffer_p tmp = cmdbuf->buffer_refs[item_loc];
            psb__information_message("RAR: found same drm buffer with different psb buffer, link them\n",
                               tmp, buf);
            while ((tmp->next != NULL)) {
                tmp = tmp->next;
                if (tmp == buf) /* found same buffer */
                    break;
            }

            if (tmp != buf) {
                tmp->next = buf; /* link it */
                buf->status = psb_bs_queued;
                buf->next = NULL;
            } else {
                psb__information_message("RAR: buffer aleady in the list, skip\n",
                                   tmp, buf);
            }
        }
        
	return item_loc;
}

/* Creates a relocation record for a DWORD in the mapped "cmdbuf" at address
 * "addr_in_cmdbuf"
 * The relocation is based on the device virtual address of "ref_buffer"
 * "buf_offset" is be added to the device virtual address, and the sum is then
 * right shifted with "align_shift".
 * "mask" determines which bits of the target DWORD will be updated with the so
 * constructed address. The remaining bits will be filled with bits from "background".
 */
void psb_cmdbuf_add_relocation( psb_cmdbuf_p cmdbuf,
                                uint32_t *addr_in_cmdbuf,
                                psb_buffer_p ref_buffer,
                                uint32_t buf_offset,
                                uint32_t mask,
                                uint32_t background,
                                uint32_t align_shift,
                                uint32_t dst_buffer) /* 0 = reloc buf, 1 = cmdbuf, 2 = for host reloc */
{
    struct drm_psb_reloc *reloc = cmdbuf->reloc_idx;
    uint64_t presumed_offset = wsbmBOOffsetHint(ref_buffer->drm_buf);

    /* Check that address is within buffer range */
    if (dst_buffer)
    {
        ASSERT( ((void *) (addr_in_cmdbuf)) >= cmdbuf->cmd_base );
        ASSERT( ((void *) (addr_in_cmdbuf)) < LLDMA_END(cmdbuf) );
        reloc->where = addr_in_cmdbuf - (uint32_t *) cmdbuf->cmd_base; /* Location in DWORDs */
    }
    else
    {
        ASSERT( ((void *) (addr_in_cmdbuf)) >= cmdbuf->MTX_msg );
        ASSERT( ((void *) (addr_in_cmdbuf)) < MTXMSG_END(cmdbuf) );
        reloc->where = addr_in_cmdbuf - (uint32_t *) cmdbuf->MTX_msg; /* Location in DWORDs */
    }

    reloc->buffer = psb_cmdbuf_buffer_ref( cmdbuf, ref_buffer );
    ASSERT( reloc->buffer != -1 );
    
    reloc->reloc_op = PSB_RELOC_OP_OFFSET;
    
#ifdef DEBUG_TRACE
psb__trace_message("[RE] Reloc at offset %08x (%08x), offset = %08x background = %08x buffer = %d (%08x)\n", reloc->where, reloc->where << 2, buf_offset, background, reloc->buffer, presumed_offset);
#endif

    if (presumed_offset)
    {
        uint32_t new_val =  presumed_offset + buf_offset;
        new_val = ((new_val >> align_shift) << (align_shift << PSB_RELOC_ALSHIFT_SHIFT));
        new_val = (background & ~mask) | (new_val & mask);
        *addr_in_cmdbuf = new_val;
    } 
    else
    {
        *addr_in_cmdbuf = PSB_RELOC_MAGIC;
    }
	
    reloc->mask = mask;
    reloc->shift = align_shift << PSB_RELOC_ALSHIFT_SHIFT;
    reloc->pre_add =  buf_offset;
    reloc->background = background;
    reloc->dst_buffer = dst_buffer;
    cmdbuf->reloc_idx++;

    ASSERT( ((void *) (cmdbuf->reloc_idx)) < RELOC_END(cmdbuf) );
}

/*
 * Advances "obj_context" to the next cmdbuf
 *
 * Returns 0 on success
 */
int psb_context_get_next_cmdbuf( object_context_p obj_context )
{
    psb_cmdbuf_p cmdbuf;
    int ret;
    
    if (obj_context->cmdbuf)
    {
        return 0;
    }

    obj_context->cmdbuf_current++;
    if (obj_context->cmdbuf_current >= PSB_MAX_CMDBUFS)
    {
        obj_context->cmdbuf_current = 0;
    }
    cmdbuf = obj_context->cmdbuf_list[obj_context->cmdbuf_current];
    ret = psb_cmdbuf_reset( cmdbuf );
    if (!ret)
    {
        /* Success */
        obj_context->cmdbuf = cmdbuf;
    }
    return ret;
}


static unsigned
psbTimeDiff(struct timeval *now, struct timeval *then)
{
    long long val;

    val = now->tv_sec - then->tv_sec;
    val *= 1000000LL;
    val += now->tv_usec;
    val -= then->tv_usec;
    if (val < 1LL)
	val = 1LL;

    return (unsigned) val;
}

/*
 * This is the user-space do-it-all interface to the drm cmdbuf ioctl.
 * It allows different buffers as command- and reloc buffer. A list of
 * cliprects to apply and whether to copy the clipRect content to all
 * scanout buffers (damage = 1).
 */
/*
 * Don't add debug statements in this function, it gets called with the
 * DRM lock held and output to an X terminal can cause X to deadlock
 */
static int
psbDRMCmdBuf(int fd, int ioctl_offset, psb_buffer_p *buffer_list,int buffer_count,unsigned cmdBufHandle,
             unsigned cmdBufOffset, unsigned cmdBufSize,
             unsigned relocBufHandle, unsigned relocBufOffset,
             unsigned numRelocs, drm_clip_rect_t * clipRects, int damage,
             unsigned engine, unsigned fence_flags,struct psb_ttm_fence_rep *fence_arg)
{
    drm_psb_cmdbuf_arg_t ca;
    struct psb_validate_arg *arg_list;
    int i;    
    int ret;
    struct timeval then, now;
    Bool have_then = FALSE;
    uint64_t mask = PSB_GPU_ACCESS_MASK;

    arg_list = (struct psb_validate_arg *) malloc(sizeof(struct psb_validate_arg)*buffer_count);
    if (arg_list == NULL) {
        psb__error_message("Malloc failed \n");
        return -ENOMEM;
    }
    
    for( i = 0; i < buffer_count; i++)
    {
        struct psb_validate_arg *arg = &(arg_list[i]);
        struct psb_validate_req *req = &arg->d.req;

        memset(arg, 0, sizeof(*arg));
        req->next = (unsigned long) &(arg_list[i+1]);
            
        req->buffer_handle = wsbmKBufHandle(wsbmKBuf(buffer_list[i]->drm_buf));
        req->group = 0;
        req->set_flags = (PSB_GPU_ACCESS_READ | PSB_GPU_ACCESS_WRITE) & mask;
        req->clear_flags = (~(PSB_GPU_ACCESS_READ | PSB_GPU_ACCESS_WRITE)) & mask;

        req->presumed_gpu_offset = (uint64_t)wsbmBOOffsetHint(buffer_list[i]->drm_buf);
        req->presumed_flags = PSB_USE_PRESUMED;
    }
    arg_list[buffer_count-1].d.req.next = 0;
    
    ca.buffer_list = (uint64_t)((unsigned long)arg_list);    
    ca.clip_rects = (uint64_t)((unsigned long)clipRects);
    ca.scene_arg = 0;
    ca.fence_arg = (uint64_t) ((unsigned long)fence_arg);
    
    ca.ta_flags = 0;
    
    ca.ta_handle = 0;
    ca.ta_offset = 0;
    ca.ta_size = 0;

    ca.oom_handle = 0;
    ca.oom_offset = 0;
    ca.oom_size = 0;
    
    ca.cmdbuf_handle = cmdBufHandle;
    ca.cmdbuf_offset = cmdBufOffset;
    ca.cmdbuf_size = cmdBufSize;
	
    ca.reloc_handle = relocBufHandle;
    ca.reloc_offset = relocBufOffset;
    ca.num_relocs = numRelocs;
	
    ca.damage = damage;
    ca.fence_flags = fence_flags;
    ca.engine = engine;

    ca.feedback_ops = 0;
    ca.feedback_handle = 0;
    ca.feedback_offset = 0;
    ca.feedback_breakpoints = 0;
    ca.feedback_size = 0;

#if 1
psb__information_message("PSB submit: buffer_list   = %08x\n", ca.buffer_list);
psb__information_message("PSB submit: clip_rects    = %08x\n", ca.clip_rects);
psb__information_message("PSB submit: cmdbuf_handle = %08x\n", ca.cmdbuf_handle);
psb__information_message("PSB submit: cmdbuf_offset = %08x\n", ca.cmdbuf_offset);
psb__information_message("PSB submit: cmdbuf_size   = %08x\n", ca.cmdbuf_size);
psb__information_message("PSB submit: reloc_handle  = %08x\n", ca.reloc_handle);
psb__information_message("PSB submit: reloc_offset  = %08x\n", ca.reloc_offset);
psb__information_message("PSB submit: num_relocs    = %08x\n", ca.num_relocs);
psb__information_message("PSB submit: engine        = %08x\n", ca.engine);
psb__information_message("PSB submit: fence_flags   = %08x\n", ca.fence_flags);
#endif

    /*
     * X server Signals will clobber the kernel time out mechanism.
     * we need a user-space timeout as well.
     */
    do {
        ret = drmCommandWrite(fd, ioctl_offset, &ca, sizeof(ca));
        if (ret == EAGAIN) {
            if (!have_then) {
                if (gettimeofday(&then, NULL)) {
                    psb__error_message("Gettimeofday error.\n");
                    break;
                }
                
                have_then = TRUE;
            }
            if (gettimeofday(&now, NULL)) {
                psb__error_message("Gettimeofday error.\n");
                break;
            }
            
        }
    } while ((ret == EAGAIN) && (psbTimeDiff(&now, &then) < PSB_TIMEOUT_USEC));

    psb__information_message("command write return is %d\n", ret);

    if (ret)
    {
        psb__information_message("command write return is %d\n", ret);
        goto out;
    }

    for( i = 0; i < buffer_count; i++)
    {
        struct psb_validate_arg *arg = &(arg_list[i]);
        struct psb_validate_rep *rep = &arg->d.rep;
        
        if (!arg->handled) {
            ret = -EFAULT;
            goto out;
        }
        if (arg->ret != 0)
        {
            ret = arg->ret;
            goto out;
        }
        wsbmUpdateKBuf(wsbmKBuf(buffer_list[i]->drm_buf),
                       rep->gpu_offset, rep->placement, rep->fence_type_mask);
    }

  out:
    free(arg_list);
    for( i = 0; i < buffer_count; i++)
    {
        /*
         * Buffer no longer queued in userspace
         */
        psb_buffer_p tmp = buffer_list[i];
        
        /*
         * RAR slice buffer/surface buffer are share one BO, and then only one in
         * buffer_list, but they are linked in psb_cmdbuf_buffer_ref
         
         */
        if (buffer_list[i]->rar_handle==0)
            tmp->next = NULL; /* don't loop for non RAR buffer, "next" may be not initialized  */
        
        do {
            psb_buffer_p p = tmp;

            tmp = tmp->next;
            switch (p->status) {
            case psb_bs_queued:
                p->status = psb_bs_ready;
                break;

            case psb_bs_abandoned:
                psb_buffer_destroy(p);
                free(p);
                break;

            default:
                /* Not supposed to happen */
                ASSERT(0);
            }
        } while (tmp);
    }
    
    return ret;
}

#ifdef DEBUG_TRACE

#define DBH(fmt, arg...)        psb__trace_message(fmt, ##arg)
#define DB(fmt, arg1, arg...)        psb__trace_message("[%08x] %08x = " fmt, ((void *) arg1) - cmd_start, *arg1, ##arg)

/* See also MsvdxGpuSim() in msvdxgpu.c */
static void debug_dump_cmdbuf(uint32_t *cmd_idx, uint32_t cmd_size_in_bytes)
{
    uint32_t cmd_size = cmd_size_in_bytes / sizeof(uint32_t);
    uint32_t *cmd_end = cmd_idx + cmd_size;
    void *cmd_start = cmd_idx;
    DBH("CMD BUFFER [%08x] - [%08x], %08x bytes, %08x dwords\n", (uint32_t) cmd_idx, cmd_end, cmd_size_in_bytes, cmd_size);
while(cmd_idx < cmd_end)
{
    uint32_t cmd = *cmd_idx;
/* What about CMD_MAGIC_BEGIN ?*/
    switch(cmd & CMD_MASK)
    {
      case CMD_HEADER:
      {
          uint32_t context = cmd & CMD_HEADER_CONTEXT_MASK;
          DB("CMD_HEADER context = %08x\n", cmd_idx, context);
          cmd_idx++;
          DB("StatusBufferAddress\n", cmd_idx);
          cmd_idx++;
          DB("PreloadSave\n", cmd_idx);
          cmd_idx++;
          DB("PreloadRestore\n", cmd_idx);
          cmd_idx++;
          break;
      }
      case CMD_REGVALPAIR_WRITE:
      {
          uint32_t count = (cmd & CMD_REGVALPAIR_COUNT_MASK) >> CMD_REGVALPAIR_COUNT_SHIFT;
          DB("CMD_REGVALPAIR_WRITE count = %08x\n", cmd_idx, count);
          cmd_idx++;
          
          while(count--)
          {
              DB("reg address\n", cmd_idx);
              cmd_idx++;
              DB("value\n", cmd_idx);
              cmd_idx++;
          }
          break;
      }
      case CMD_RENDEC_WRITE:
      {
          uint32_t encoding;
          uint32_t count = (cmd & CMD_RENDEC_COUNT_MASK) >> CMD_RENDEC_COUNT_SHIFT;
          DB("CMD_RENDEC_WRITE count = %08x\n", cmd_idx, count);
          cmd_idx++;

          DB("RENDEC_SL_HDR\n", cmd_idx);
          cmd_idx++;

          DB("RENDEC_SL_NULL\n", cmd_idx);
          cmd_idx++;
          
          do
          {
              uint32_t chk_hdr = *cmd_idx;
              count = 1 + ((chk_hdr & 0x07FF0000) >> 16);
              uint32_t start_address = (chk_hdr & 0x0000FFF0) >> 4; 
              encoding = (chk_hdr & 0x07);
              if ((count == 1) && (encoding == 7))
              {
                  count = 0;
                  DB("SLICE_SEPARATOR\n", cmd_idx);
              }
              else
              {
                  DB("RENDEC_CK_HDR #symbols = %d address = %08x encoding = %01x\n", cmd_idx, count, start_address, encoding);
            }
              cmd_idx++;
          
              while(count && (count < 0x1000))
              {
                  DB("value\n", cmd_idx);
                  cmd_idx++;

                  count -= 2;
              }
        }
        while (encoding != 0x07);
          
          break;
      }
      case CMD_COMPLETION:
      {
          if (*cmd_idx == PSB_RELOC_MAGIC)
          {
              DB("CMD_(S)LLDMA (assumed)\n", cmd_idx);
              cmd_idx++;
              
          }
          else
          {
              DB("CMD_COMPLETION\n", cmd_idx);
              cmd_idx++;
          
//              DB("interrupt\n", cmd_idx);
//              cmd_idx++;
          }
          break;
      }
      case CMD_LLDMA:
      {
          DB("CMD_LLDMA\n", cmd_idx);
          cmd_idx++;
          break;
      }
      case CMD_SLLDMA:
      {
          DB("CMD_SLLDMA\n", cmd_idx);
          cmd_idx++;
          break;
      }
      case CMD_SR_SETUP:
      {
          DB("CMD_SR_SETUP\n", cmd_idx);
          cmd_idx++;
          DB("offset in bits\n", cmd_idx);
          cmd_idx++;
          DB("size in bytes\n", cmd_idx);
          cmd_idx++;
          break;
      }
      default:
          if (*cmd_idx == PSB_RELOC_MAGIC)
          {
              DB("CMD_(S)LLDMA (assumed)\n", cmd_idx);
              cmd_idx++;
              
          }
          else
          {
              DB("*** Unknown command ***\n", cmd_idx);
              cmd_idx++;
          }
          break;
    } /* switch */
} /* while */
}
#endif


int psb_fence_destroy(struct _WsbmFenceObject *pFence)
{
    wsbmFenceUnreference(&pFence);

    return 0;
}

struct _WsbmFenceObject *
psb_fence_wait(psb_driver_data_p driver_data,
               struct psb_ttm_fence_rep *fence_rep, int *status)

{
    struct _WsbmFenceObject *fence = NULL;
    int ret = -1;
    
    /* copy fence information */
    if (fence_rep->error != 0) {
        psb__error_message("drm failed to create a fence"
                           " and has idled the HW\n");
        DEBUG_FAILURE_RET;
        return NULL;
    }

    fence = wsbmFenceCreate(driver_data->fence_mgr, fence_rep->fence_class,
                            fence_rep->fence_type,
                            (void *)fence_rep->handle,
                            0);
    if (fence) 
        *status = wsbmFenceFinish(fence,fence_rep->fence_type,0);
    
    return fence;
}


#ifdef DEBUG_TRACE
uint32_t debug_cmd_start[MAX_CMD_COUNT];
uint32_t debug_cmd_size[MAX_CMD_COUNT];
uint32_t debug_cmd_count;
uint32_t debug_lldma_count;
uint32_t debug_lldma_start;

#define MAX_DUMP_COUNT 	20
const char * debug_dump_name[MAX_DUMP_COUNT];
psb_buffer_p debug_dump_buf[MAX_DUMP_COUNT];
uint32_t     debug_dump_offset[MAX_DUMP_COUNT];
uint32_t     debug_dump_size[MAX_DUMP_COUNT];
uint32_t     debug_dump_count = 0;
#endif

#ifdef DEBUG_TRACE
#define DW(wd, sym, to, from) psb__debug_w(((uint32_t *)pasDmaList)[wd], "LLDMA: " #sym " = %d\n", to, from);
#define DWH(wd, sym, to, from) psb__debug_w(((uint32_t *)pasDmaList)[wd], "LLDMA: " #sym " = %08x\n", to, from);
static void psb__debug_w(uint32_t val, char *fmt, uint32_t bit_to, uint32_t bit_from)
{
    if (bit_to < 31)
    {
        val &= ~(0xffffffff << (bit_to+1));
    }
    val = val >> bit_from;
    psb__trace_message(fmt, val);
}

static uint32_t g_hexdump_offset = 0;

static void psb__hexdump2(unsigned char *p, int offset, int size)
{
    if (offset + size > 8)
        size = 8 - offset;
    psb__trace_message("[%04x]", g_hexdump_offset);
    g_hexdump_offset += offset;
    g_hexdump_offset += size;
    while( offset-- > 0)
    {
        psb__trace_message(" --");
    }
    while( size-- > 0)
    {
        psb__trace_message(" %02x", *p++);
    }
    psb__trace_message("\n");
}

static void psb__hexdump(void *addr, int size)
{
    unsigned char *p = (unsigned char *) addr;
    
    int offset = g_hexdump_offset % 8;
    g_hexdump_offset -= offset;
    if (offset)
    {
        psb__hexdump2(p, offset, size);
        size -= 8 - offset;
        p += 8 - offset;
    }

    while( 1 )
    {
        if (size < 8)
        {
            if (size > 0)
            {
                psb__hexdump2(p, 0, size);
            }
            return;
        }
        psb__trace_message("[%04x] %02x %02x %02x %02x %02x %02x %02x %02x\n", g_hexdump_offset, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        p += 8;
        size -= 8;
        g_hexdump_offset += 8;
    }
}

void psb__debug_schedule_hexdump(const char *name, psb_buffer_p buf, uint32_t offset, uint32_t size)
{
    ASSERT(debug_dump_count < MAX_DUMP_COUNT);
    debug_dump_name[debug_dump_count] = name;
    debug_dump_buf[debug_dump_count] = buf;
    debug_dump_offset[debug_dump_count] = offset;
    debug_dump_size[debug_dump_count] = size;
    debug_dump_count++;
}

#endif


/*
 * Closes the last segment
 */
static void psb_cmdbuf_close_segment( psb_cmdbuf_p cmdbuf )
{
    uint32_t bytes_used = ((void *) cmdbuf->cmd_idx - cmdbuf->cmd_start) % MTX_SEG_SIZE;
    void *segment_start = (void *) cmdbuf->cmd_idx - bytes_used;
    uint32_t lldma_record_offset = psb_cmdbuf_lldma_create(cmdbuf, 
                                          &(cmdbuf->buf), (segment_start - cmdbuf->cmd_base) /* offset */,
                                          bytes_used,
                                          0 /* destination offset */,
                                          LLDMA_TYPE_RENDER_BUFF_MC);
    uint32_t cmd = CMD_NEXT_SEG;
    RELOC_SHIFT4( *cmdbuf->last_next_segment_cmd, lldma_record_offset, cmd, &(cmdbuf->buf));
    *(cmdbuf->last_next_segment_cmd+1) = bytes_used;
}

int psb_context_submit_deblock( object_context_p obj_context,
				psb_buffer_p source_buf,
				psb_buffer_p colocate_buffer,
				uint32_t picture_width_in_mb,
				uint32_t frame_height_in_mb,
				uint32_t chroma_offset )
{
    psb_cmdbuf_p cmdbuf = obj_context->cmdbuf;
    uint32_t msg_size = FW_DXVA_DEBLOCK_SIZE;
    uint32_t *msg = cmdbuf->MTX_msg;
    DEBLOCKPARAMS* pdbParams;

    psb__information_message("Send two pass deblock cmd\n");
    if(cmdbuf->cmd_count) {
        psb__information_message("two pass deblock cmdbuf has render msg!\n");
	return 1;
    }

    cmdbuf->deblock_count++;
    memset(msg, 0, msg_size);

    MEMIO_WRITE_FIELD(msg, FWRK_GENMSG_SIZE, 16); /* Deblock message size is 16 bytes */
    MEMIO_WRITE_FIELD(msg, FWRK_GENMSG_ID, DXVA_MSGID_DEBLOCK);

    MEMIO_WRITE_FIELD(msg, FW_DXVA_DEBLOCK_CONTEXT, obj_context->msvdx_context);
    MEMIO_WRITE_FIELD(msg, FW_DXVA_DEBLOCK_FLAGS, FW_DXVA_RENDER_HOST_INT );
    
    pdbParams = (DEBLOCKPARAMS*) (msg + 16 / sizeof(uint32_t));

    pdbParams->handle = wsbmKBufHandle(wsbmKBuf(cmdbuf->regio_buf.drm_buf));
    //printf("regio buffer size is 0x%x\n", cmdbuf->regio_size);
    pdbParams->buffer_size = cmdbuf->regio_size;
    pdbParams->ctxid = obj_context->msvdx_context;

    return 0;
}


/*
 * Submits the current cmdbuf
 *
 * Returns 0 on success
 */
int psb_context_submit_cmdbuf( object_context_p obj_context )
{
    psb_cmdbuf_p cmdbuf = obj_context->cmdbuf;
    uint32_t cmdbuffer_size = (void *) cmdbuf->cmd_idx - cmdbuf->cmd_start; // In bytes

    if (cmdbuf->last_next_segment_cmd)
    {
        cmdbuffer_size = cmdbuf->first_segment_size;
        psb_cmdbuf_close_segment( cmdbuf );
    }

    uint32_t msg_size = FW_DXVA_RENDER_SIZE;
    uint32_t *msg = cmdbuf->MTX_msg + cmdbuf->cmd_count * FW_DXVA_RENDER_SIZE;

#ifdef DEBUG_TRACE
    debug_cmd_start[cmdbuf->cmd_count] = cmdbuf->cmd_start - cmdbuf->cmd_base;
    debug_cmd_size[cmdbuf->cmd_count] = (void *) cmdbuf->cmd_idx - cmdbuf->cmd_start;
    debug_cmd_count = cmdbuf->cmd_count+1;
#endif

    cmdbuf->cmd_count++;
    memset(msg, 0, msg_size);

    *cmdbuf->cmd_idx = 0; // Add a trailing 0 just in case.
    ASSERT(cmdbuffer_size < CMD_SIZE);
    ASSERT((void *) cmdbuf->cmd_idx < CMD_END(cmdbuf));

    MEMIO_WRITE_FIELD(msg, FWRK_GENMSG_SIZE,                  FW_DXVA_RENDER_SIZE);
    MEMIO_WRITE_FIELD(msg, FWRK_GENMSG_ID,                    DXVA_MSGID_RENDER);
    /* TODO: Need to make context more unique */
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_CONTEXT,            obj_context->msvdx_context );

    /* Point to CMDBUFFER */
    uint32_t lldma_record_offset = psb_cmdbuf_lldma_create(cmdbuf, 
                                            &(cmdbuf->buf), (cmdbuf->cmd_start - cmdbuf->cmd_base) /* offset */,
                                            cmdbuffer_size,
                                            0 /* destination offset */,
                                            (obj_context->video_op == psb_video_vld) ? LLDMA_TYPE_RENDER_BUFF_VLD : LLDMA_TYPE_RENDER_BUFF_MC);
    /* This is the last relocation */
    RELOC_MSG( *(msg + (FW_DXVA_RENDER_LLDMA_ADDRESS_OFFSET/sizeof(uint32_t))), 
               lldma_record_offset, &(cmdbuf->buf));
    
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_BUFFER_SIZE,          cmdbuffer_size); // In bytes
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_OPERATING_MODE,       obj_context->operating_mode);
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_LAST_MB_IN_FRAME,     obj_context->last_mb);
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_FIRST_MB_IN_SLICE,    obj_context->first_mb);
    MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_FLAGS,                obj_context->flags);

#ifdef DEBUG_TRACE
    debug_lldma_count = (cmdbuf->lldma_idx - cmdbuf->lldma_base) / sizeof(DMA_sLinkedList);
    debug_lldma_start = cmdbuf->lldma_base - cmdbuf->cmd_base;
    /* Indicate last LLDMA record (for debugging) */
    ((uint32_t *)cmdbuf->lldma_idx)[1] = 0;
#endif

    cmdbuf->cmd_start = cmdbuf->cmd_idx;

#ifdef DEBUG_TRACE
    return psb_context_flush_cmdbuf( obj_context ); 
#else
    if ((cmdbuf->cmd_count >= MAX_CMD_COUNT) ||
        (MTXMSG_END(cmdbuf) - (void *) msg < MTXMSG_MARGIN) ||
        (CMD_END(cmdbuf) - (void *) cmdbuf->cmd_idx < CMD_MARGIN) ||
        (LLDMA_END(cmdbuf) - cmdbuf->lldma_idx < LLDMA_MARGIN) ||
        (RELOC_END(cmdbuf) - (void *) cmdbuf->reloc_idx < RELOC_MARGIN))
    {
        return psb_context_flush_cmdbuf( obj_context ); 
    }
#endif    
    return 0;
}

/*
 * Flushes all cmdbufs
 */
int psb_context_flush_cmdbuf( object_context_p obj_context )
{
    psb_cmdbuf_p cmdbuf = obj_context->cmdbuf;
    psb_driver_data_p driver_data = obj_context->driver_data;
    unsigned int fence_flags;
    //unsigned int fence_handle = 0;
    struct psb_ttm_fence_rep fence_rep;
    unsigned int reloc_offset;
    unsigned int num_relocs;
    int ret;

    if ((NULL == cmdbuf) || ((0 == cmdbuf->cmd_count) && (0 == cmdbuf->deblock_count)))
    {
        return 0; // Nothing to do
    }

    uint32_t msg_size = 0;
    uint32_t *msg = cmdbuf->MTX_msg;
    int i;

    /* LOCK */
    ret = LOCK_HARDWARE(driver_data);
    if (ret)
    {
        UNLOCK_HARDWARE(driver_data);
        DEBUG_FAILURE_RET;
        return ret;
    }
    
    for(i = 1; i <= cmdbuf->cmd_count; i++)
    {
        uint32_t flags = MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_FLAGS);
        
        /* Update flags */
        int bBatchEnd = (i == cmdbuf->cmd_count + cmdbuf->deblock_count);

        flags |= 
            (bBatchEnd ? (FW_DXVA_RENDER_HOST_INT | FW_DXVA_LAST_SLICE_OF_EXT_DMA)    : FW_DXVA_RENDER_NO_RESPONCE_MSG) |
            (obj_context->video_op == psb_video_vld    ? FW_DXVA_RENDER_IS_VLD_NOT_MC : 0);

	if( i == cmdbuf->cmd_count )
            flags |= FW_DXVA_LAST_SLICE_OF_EXT_DMA;

        MEMIO_WRITE_FIELD(msg, FW_DXVA_RENDER_FLAGS, flags);

#ifdef DEBUG_TRACE
psb__trace_message("MSG BUFFER_SIZE       = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_BUFFER_SIZE) );
psb__trace_message("MSG OPERATING_MODE    = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_OPERATING_MODE) );
psb__trace_message("MSG LAST_MB_IN_FRAME  = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_LAST_MB_IN_FRAME) );
psb__trace_message("MSG FIRST_MB_IN_SLICE = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_FIRST_MB_IN_SLICE) );
psb__trace_message("MSG FLAGS             = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_FLAGS) );

psb__information_message("MSG BUFFER_SIZE       = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_BUFFER_SIZE) );
psb__information_message("MSG OPERATING_MODE    = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_OPERATING_MODE) );
psb__information_message("MSG LAST_MB_IN_FRAME  = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_LAST_MB_IN_FRAME) );
psb__information_message("MSG FIRST_MB_IN_SLICE = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_FIRST_MB_IN_SLICE) );
psb__information_message("MSG FLAGS             = %08x\n", MEMIO_READ_FIELD(msg, FW_DXVA_RENDER_FLAGS) );
#endif

#if 0  /* todo */
        /* Update SAREA */
        driver_data->psb_sarea->msvdx_context = obj_context->msvdx_context;
#endif
        
        msg += FW_DXVA_RENDER_SIZE / sizeof(uint32_t);
        msg_size += FW_DXVA_RENDER_SIZE;
    }

    /* Assume deblock message is following render messages and no more render message behand deblock message */
    for(i = 1; i <= cmdbuf->deblock_count; i++)
    {
        msg_size += FW_DXVA_DEBLOCK_SIZE;
    }
 
    /* Now calculate the total number of relocations */
    reloc_offset = cmdbuf->reloc_base - cmdbuf->MTX_msg;
    num_relocs = (((void *) cmdbuf->reloc_idx) - cmdbuf->reloc_base) / sizeof(struct drm_psb_reloc);

#ifdef DEBUG_TRACE
psb__information_message("Cmdbuf MTXMSG size = %08x [%08x]\n", msg_size, MTXMSG_SIZE);
psb__information_message("Cmdbuf CMD size = %08x - %d[%08x]\n",  (void *) cmdbuf->cmd_idx - cmdbuf->cmd_base, cmdbuf->cmd_count, CMD_SIZE);
psb__information_message("Cmdbuf LLDMA size = %08x [%08x]\n", cmdbuf->lldma_idx - cmdbuf->lldma_base, LLDMA_SIZE);
psb__information_message("Cmdbuf RELOC size = %08x [%08x]\n", num_relocs * sizeof(struct drm_psb_reloc), RELOC_SIZE);
#endif
    
    psb_cmdbuf_unmap( cmdbuf );

#ifdef DEBUG_TRACE
    psb__trace_message(NULL); /* Flush trace */
#endif
    
    ASSERT( NULL == cmdbuf->MTX_msg );
    ASSERT( NULL == cmdbuf->reloc_base );

#ifdef DEBUG_TRACE
    fence_flags = 0;
#else
    fence_flags = DRM_PSB_FENCE_NO_USER;
#endif    

    /* cmdbuf will be validated as part of the buffer list */
    /* Submit */
#if 1
    wsbmWriteLockKernelBO();
    ret = psbDRMCmdBuf(driver_data->drm_fd, driver_data->execIoctlOffset,cmdbuf->buffer_refs,
		    cmdbuf->buffer_refs_count,
		    wsbmKBufHandle(wsbmKBuf(cmdbuf->reloc_buf.drm_buf)),
		    0, msg_size,
		    wsbmKBufHandle(wsbmKBuf(cmdbuf->reloc_buf.drm_buf)),
		    reloc_offset, num_relocs,
		    0 /* clipRects */, 0, PSB_ENGINE_VIDEO, fence_flags, &fence_rep);
    wsbmWriteUnlockKernelBO();
    UNLOCK_HARDWARE(driver_data);
#else
    ret = 1;
#endif
    if (ret)
    {
        obj_context->cmdbuf = NULL;
        obj_context->slice_count++;

        DEBUG_FAILURE_RET;
        return ret;
    }

    
#ifdef DEBUG_TRACE
static int error_count = 0;
    int status = 0;
    struct _WsbmFenceObject *fence=NULL;
#if 0
    fence = psb_fence_wait(driver_data, &fence_rep, &status);
    psb__information_message("psb_fence_wait returns: %d (fence=0x%08x)\n",status, fence);
#endif    
    psb_buffer_map( &cmdbuf->buf, &cmdbuf->cmd_base );

    psb__information_message("lldma_count = %d\n", debug_lldma_count);
    for(i = 0; i < debug_lldma_count; i++)
    {
        DMA_sLinkedList* pasDmaList = (DMA_sLinkedList*) (cmdbuf->cmd_base + debug_lldma_start);
        pasDmaList += i;
#ifdef DEBUG_TRACE
psb__trace_message("\nLLDMA record at offset %08x\n", ((void*)pasDmaList) - cmdbuf->cmd_base);
DW(0, BSWAP,    31, 31)
DW(0, DIR,    30, 30)
DW(0, PW,    29, 28)
DW(1, List_FIN, 31, 31)
DW(1, List_INT, 30, 30)
DW(1, PI,    18, 17)
DW(1, INCR,    16, 16)
DW(1, LEN,    15, 0)
DWH(2, ADDR,    22, 0)
DW(3, ACC_DEL,    31, 29)
DW(3, BURST,    28, 26)
DWH(3, EXT_SA,    3, 0)
DW(4, 2D_MODE,    16, 16)
DW(4, REP_COUNT, 10, 0)
DWH(5, LINE_ADD_OFF, 25, 16)
DW(5, ROW_LENGTH, 9, 0)
DWH(6, SA, 31, 0)
DWH(7, LISTPTR, 27, 0)
#endif
    }

    psb__information_message("cmd_count = %d\n", debug_cmd_count);
    for(i = 0; i < debug_cmd_count; i++)
    {
        psb__information_message("start = %08x size = %08x\n", debug_cmd_start[i], debug_cmd_size[i]);
        debug_dump_cmdbuf( (uint32_t *) (cmdbuf->cmd_base + debug_cmd_start[i]), debug_cmd_size[i] );
    }
    psb_buffer_unmap( &cmdbuf->buf );
    cmdbuf->cmd_base = NULL;

    psb__trace_message("debug_dump_count = %d\n", debug_dump_count);
    for(i = 0; i < debug_dump_count; i++)
    {
        void *buf_addr;
        psb__trace_message("Buffer %d = '%s' offset = %08x size = %08x\n", i, debug_dump_name[i], debug_dump_offset[i], debug_dump_size[i]);
        psb_buffer_map( debug_dump_buf[i], &buf_addr );
        g_hexdump_offset = 0;
        psb__hexdump( buf_addr + debug_dump_offset[i], debug_dump_size[i]);
        psb_buffer_unmap( debug_dump_buf[i] );
    }
    debug_dump_count = 0;

    if (status)
    {
        psb__error_message("RENDERING ERROR FRAME=%03d SLICE=%02d status=%d\n", obj_context->frame_count, obj_context->slice_count, status);
        error_count++;
	ASSERT(status != 2);
        ASSERT(error_count < 40); /* Exit on 40 errors */
    }
    if (fence)
        psb_fence_destroy(fence);    
#endif    
    
    obj_context->cmdbuf = NULL;
    obj_context->slice_count++;
    
    return 0;
}


typedef enum {
    MMU_GROUP0 = 0,
    MMU_GROUP1 = 1,
} MMU_GROUP;

typedef enum    {
    HOST_TO_MSVDX = 0,
    MSXDX_TO_HOST = 1,
} DMA_DIRECTION;

typedef struct {
	IMG_UINT32 ui32DevDestAddr ;	/* destination address */
	DMA_ePW	ePeripheralWidth;
	DMA_ePeriphIncrSize	ePeriphIncrSize;
	DMA_ePeriphIncr	ePeriphIncr;
	IMG_BOOL		bSynchronous;
	MMU_GROUP		eMMUGroup;
	DMA_DIRECTION	eDMADir;
	DMA_eBurst		eDMA_eBurst;
} DMA_DETAIL_LOOKUP;


static const DMA_DETAIL_LOOKUP DmaDetailLookUp[] =
{
	/* LLDMA_TYPE_VLC_TABLE */ { 	REG_MSVDX_VEC_VLC_OFFSET  ,
									DMA_PWIDTH_16_BIT,	/* 16 bit wide data*/
									DMA_PERIPH_INCR_4,	/* Incrament the dest by 32 bits */
									DMA_PERIPH_INCR_ON,
									IMG_TRUE,
									MMU_GROUP0,
									HOST_TO_MSVDX,
									DMA_BURST_2
								},
	/* LLDMA_TYPE_BITSTREAM */ {
									( REG_MSVDX_VEC_OFFSET + MSVDX_VEC_CR_VEC_SHIFTREG_STREAMIN_OFFSET  ),
									DMA_PWIDTH_8_BIT,
									DMA_PERIPH_INCR_1,
									DMA_PERIPH_INCR_OFF,
									IMG_FALSE,
									MMU_GROUP0,
									HOST_TO_MSVDX,
									DMA_BURST_4
								},
	/*LLDMA_TYPE_RESIDUAL*/		{
									(REG_MSVDX_VDMC_OFFSET + MSVDX_VDMC_CR_VDMC_RESIDUAL_DIRECT_INSERT_DATA_OFFSET),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_FALSE,
									MMU_GROUP1,
									HOST_TO_MSVDX,
									DMA_BURST_2
								},

	/*LLDMA_TYPE_RENDER_BUFF_MC*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,
									MMU_GROUP1,
									HOST_TO_MSVDX,
									DMA_BURST_1		/* Into MTX */
								},
	/*LLDMA_TYPE_RENDER_BUFF_VLD*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,
									MMU_GROUP0,
									HOST_TO_MSVDX,
									DMA_BURST_1		/* Into MTX */
								},
	/*LLDMA_TYPE_MPEG4_FESTATE_SAVE*/{
									(REG_MSVDX_VEC_RAM_OFFSET + 0x700 ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_4,	
									DMA_PERIPH_INCR_ON,
									IMG_TRUE,
									MMU_GROUP0,
									MSXDX_TO_HOST,
									DMA_BURST_2		 /* From VLR */
								},
	/*LLDMA_TYPE_MPEG4_FESTATE_RESTORE*/{
									(REG_MSVDX_VEC_RAM_OFFSET + 0x700 ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_4,	
									DMA_PERIPH_INCR_ON,
									IMG_TRUE,
									MMU_GROUP0,
									HOST_TO_MSVDX,
									DMA_BURST_2		/* Into VLR */
								},
	/*LLDMA_TYPE_H264_PRELOAD_SAVE*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,	/* na */
									MMU_GROUP1,
									MSXDX_TO_HOST,
									DMA_BURST_1		/* From MTX */
								},
	/*LLDMA_TYPE_H264_PRELOAD_RESTORE*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,	/* na */
									MMU_GROUP1,
									HOST_TO_MSVDX,
									DMA_BURST_1		/* Into MTX */
								},
	/*LLDMA_TYPE_VC1_PRELOAD_SAVE*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,	/* na */
									MMU_GROUP1,
									MSXDX_TO_HOST,
									DMA_BURST_1		//2	/* From MTX */
								},
	/*LLDMA_TYPE_VC1_PRELOAD_RESTORE*/{
									(REG_MSVDX_MTX_OFFSET + MTX_CORE_CR_MTX_SYSC_CDMAT_OFFSET ),
									DMA_PWIDTH_32_BIT,
									DMA_PERIPH_INCR_1,	
									DMA_PERIPH_INCR_OFF,
									IMG_TRUE,	/* na */
									MMU_GROUP1,
									HOST_TO_MSVDX,
									DMA_BURST_1		/* Into MTX */
								},
};

#define MAX_DMA_LEN     ( 0xffff )


void psb_cmdbuf_lldma_write_cmdbuf( psb_cmdbuf_p cmdbuf,
                                 psb_buffer_p bitstream_buf,
                                 uint32_t buffer_offset,
                                 uint32_t size,
                                 uint32_t dest_offset,
                                 LLDMA_TYPE cmd)
{
    LLDMA_CMD *pLLDMACmd = (LLDMA_CMD*) cmdbuf->cmd_idx++;
    psb_cmdbuf_lldma_create_internal(cmdbuf, pLLDMACmd, bitstream_buf, buffer_offset, size,
                dest_offset, cmd);
}

uint32_t psb_cmdbuf_lldma_create( psb_cmdbuf_p cmdbuf,
                                 psb_buffer_p bitstream_buf,
                                 uint32_t buffer_offset,
                                 uint32_t size,
                                 uint32_t dest_offset,
                                 LLDMA_TYPE cmd)
{
    uint32_t lldma_record_offset = (((void*)cmdbuf->lldma_idx) - ((void *) cmdbuf->cmd_base));
    psb_cmdbuf_lldma_create_internal(cmdbuf, 0, bitstream_buf, buffer_offset, size,
                dest_offset, cmd);
    return lldma_record_offset;
}

/*
 * Write a CMD_SR_SETUP referencing a bitstream buffer to the command buffer
 */
void psb_cmdbuf_lldma_write_bitstream( psb_cmdbuf_p cmdbuf,
                                      psb_buffer_p bitstream_buf,
                                      uint32_t buffer_offset,
                                      uint32_t size_in_bytes,
                                      uint32_t offset_in_bits,
                                      uint32_t flags)
{
/*
 * We use byte alignment instead of 32bit alignment.
 * The third frame of sa10164.vc1 results in the following bitstream 
 * patttern:
 * [0000] 00 00 03 01 76 dc 04 8d
 * with offset_in_bits = 0x1e
 * This causes an ENTDEC failure because 00 00 03 is a start code
 * By byte aligning the datastream the start code will be eliminated.
 */
//don't need to change the offset_in_bits, size_in_bytes and buffer_offset
#if 0
#define ALIGNMENT	 sizeof(uint8_t)    
    uint32_t bs_offset_in_dwords    = ((offset_in_bits /8) / ALIGNMENT);
    size_in_bytes                   -= bs_offset_in_dwords * ALIGNMENT;
    offset_in_bits                  -= bs_offset_in_dwords * 8 * ALIGNMENT;
    buffer_offset                   += bs_offset_in_dwords * ALIGNMENT;
#endif

    *cmdbuf->cmd_idx++ = CMD_SR_SETUP | flags;
    *cmdbuf->cmd_idx++ = offset_in_bits;
    cmdbuf->cmd_bitstream_size = cmdbuf->cmd_idx;
    *cmdbuf->cmd_idx++ = size_in_bytes;
    
    psb_cmdbuf_lldma_write_cmdbuf( cmdbuf, bitstream_buf, buffer_offset, 
                            size_in_bytes, 0, LLDMA_TYPE_BITSTREAM );

#ifdef DEBUG_TRACE
    psb__debug_schedule_hexdump("Bitstream", bitstream_buf, buffer_offset, size_in_bytes);
#endif
}

/*
 * Chain a LLDMA bitstream command to the previous one
 */
void psb_cmdbuf_lldma_write_bitstream_chained( psb_cmdbuf_p cmdbuf,
                                      psb_buffer_p bitstream_buf,
                                      uint32_t size_in_bytes)
{
    DMA_sLinkedList* pasDmaList = (DMA_sLinkedList*) cmdbuf->lldma_last;
    uint32_t lldma_record_offset = psb_cmdbuf_lldma_create( cmdbuf, bitstream_buf, bitstream_buf->buffer_ofs, 
                            size_in_bytes, 0, LLDMA_TYPE_BITSTREAM );
    /* Update WD7 of last LLDMA record to point to this one */
    RELOC_SHIFT4(pasDmaList->ui32Word_7, lldma_record_offset, 0, &(cmdbuf->buf));
    /* This touches WD1 */
    MEMIO_WRITE_FIELD(pasDmaList, DMAC_LL_LIST_FIN, 0);

#ifdef DEBUG_TRACE
    psb__debug_schedule_hexdump("Bitstream (chained)", bitstream_buf, 0, size_in_bytes);
#endif

    *(cmdbuf->cmd_bitstream_size) += size_in_bytes;
}

static void psb_cmdbuf_lldma_create_internal( psb_cmdbuf_p cmdbuf,
                 LLDMA_CMD *pLLDMACmd,
                                 psb_buffer_p bitstream_buf,
                                 uint32_t buffer_offset,
                                 uint32_t size,
                                 uint32_t dest_offset,
                                 LLDMA_TYPE cmd)
{
    const DMA_DETAIL_LOOKUP* pDmaDetail;
    IMG_UINT32 ui32DMACount, ui32LLDMA_Offset, ui32DMADestAddr, ui32Cmd;
    DMA_sLinkedList* pasDmaList;
    static IMG_UINT32 lu[] = {4,2,1};

    /* See if we will fit */
    ASSERT( cmdbuf->lldma_idx + sizeof( DMA_sLinkedList ) < LLDMA_END(cmdbuf) );

    pDmaDetail = &DmaDetailLookUp[cmd];

    ui32DMACount = size / lu[pDmaDetail->ePeripheralWidth];

    /* DMA list must be 16byte alligned if it is done in Hw */
    pasDmaList = (DMA_sLinkedList*) (cmdbuf->lldma_idx) ;
    // psaDmaList = (DMA_sLinkedList*) ((( cmdbuf->lldma_idx )+0x0f) & ~0x0f );

    /* Offset of LLDMA record in cmdbuf */
    ui32LLDMA_Offset = (IMG_UINT32)(((IMG_UINT8*)pasDmaList) -((IMG_UINT8*) cmdbuf->cmd_base));

    ASSERT( 0 == (ui32LLDMA_Offset&0xf) );

    ui32DMADestAddr = pDmaDetail->ui32DevDestAddr + dest_offset;

    /* Write the header */
    if (pLLDMACmd)
    {
         ui32Cmd = ((pDmaDetail->bSynchronous) ? CMD_SLLDMA : CMD_LLDMA );
         RELOC_SHIFT4(pLLDMACmd->ui32CmdAndDevLinAddr, ui32LLDMA_Offset, ui32Cmd, &(cmdbuf->buf));
    }

    while( ui32DMACount )
    {
        memset( pasDmaList , 0 ,sizeof(DMA_sLinkedList) );

        DMA_LL_SET_WD2(pasDmaList, ui32DMADestAddr );    

        /* DMA_LL_SET_WD6 with relocation */
        ASSERT(DMAC_LL_SA_SHIFT == 0);

        RELOC(pasDmaList->ui32Word_6, buffer_offset, bitstream_buf);

        if( ui32DMACount > MAX_DMA_LEN )
        {    
            ui32LLDMA_Offset+=sizeof(DMA_sLinkedList);

            /* DMA_LL_SET_WD7 with relocation */
            ASSERT(DMAC_LL_LISTPTR_SHIFT == 0);
            RELOC_SHIFT4(pasDmaList->ui32Word_7, ui32LLDMA_Offset, 0, &(cmdbuf->buf));
            /* This touches WD1 */
            MEMIO_WRITE_FIELD(pasDmaList, DMAC_LL_LIST_FIN, 0);
            
            DMA_LL_SET_WD1(pasDmaList, pDmaDetail->ePeriphIncr, pDmaDetail->ePeriphIncrSize, MAX_DMA_LEN );    /* size */

            ui32DMACount-= MAX_DMA_LEN;

            if(  pDmaDetail->ePeriphIncr == DMA_PERIPH_INCR_ON )
            {
                /* Update Destination pointers */
                ui32DMADestAddr += ((MAX_DMA_LEN)* lu[pDmaDetail->ePeriphIncrSize] ); 
            }

            /* Update Source Pointer */
            buffer_offset += ((MAX_DMA_LEN)*lu[pDmaDetail->ePeripheralWidth]); 
        }
        else
        {
            /* This also set LIST_FIN in WD1 to 1*/
            DMA_LL_SET_WD7(pasDmaList, IMG_NULL);                // next linked list
            DMA_LL_SET_WD1(pasDmaList,pDmaDetail->ePeriphIncr, pDmaDetail->ePeriphIncrSize, ui32DMACount );    /* size */

            ui32DMACount =0;
        }

        /* Keep pointer in case we need to chain another LLDMA command */
        cmdbuf->lldma_last = (void *) pasDmaList;

        DMA_LL_SET_WD0(pasDmaList, DMA_BSWAP_NO_SWAP, 
            (pDmaDetail->eDMADir==HOST_TO_MSVDX)?DMA_DIR_MEM_TO_PERIPH:DMA_DIR_PERIPH_TO_MEM ,
            pDmaDetail->ePeripheralWidth);

        DMA_LL_SET_WD3(pasDmaList, DMA_ACC_DEL_0, pDmaDetail->eDMA_eBurst, pDmaDetail->eMMUGroup );
        DMA_LL_SET_WD4(pasDmaList, DMA_MODE_2D_OFF, 0);    // 2d
        DMA_LL_SET_WD5(pasDmaList, 0, 0);                    // 2d


        pasDmaList++;
    }

    /* there can be up to 3 Bytes of padding after header */
    cmdbuf->lldma_idx    = (void *)pasDmaList;
}


/*
 * Create a command to set registers
 */
void psb_cmdbuf_reg_start_block( psb_cmdbuf_p cmdbuf )
{
    ASSERT(NULL == cmdbuf->rendec_block_start); /* Can't have both */

    cmdbuf->reg_start = cmdbuf->cmd_idx++;
}    

void psb_cmdbuf_reg_set_address( psb_cmdbuf_p cmdbuf, 
                                 uint32_t reg,
                                 psb_buffer_p buffer,
                                 uint32_t buffer_offset )
{
    *cmdbuf->cmd_idx++ = reg;
    RELOC(*cmdbuf->cmd_idx++, buffer_offset, buffer);
}

/*
 * Finish a command to set registers
 */
void psb_cmdbuf_reg_end_block( psb_cmdbuf_p cmdbuf )
{
    uint32_t reg_count = ((cmdbuf->cmd_idx - cmdbuf->reg_start) - 1) / 2;
    
    *cmdbuf->reg_start = CMD_REGVALPAIR_WRITE | reg_count;
    cmdbuf->reg_start = NULL;
}

typedef enum
{
    MTX_CTRL_HEADER = 0,
    RENDEC_SL_HDR,
    RENDEC_SL_NULL,
    RENDEC_CK_HDR,
} RENDEC_CHUNK_OFFSETS;

/*
 * Create a RENDEC command block
 */
void psb_cmdbuf_rendec_start_block( psb_cmdbuf_p cmdbuf )
{
    ASSERT(NULL == cmdbuf->rendec_block_start); /* Can't have both */
    cmdbuf->rendec_block_start = cmdbuf->cmd_idx;

    cmdbuf->rendec_block_start[RENDEC_SL_HDR] = 0;
    REGIO_WRITE_FIELD_LITE (cmdbuf->rendec_block_start[RENDEC_SL_HDR], RENDEC_SLICE_INFO, SL_HDR_CK_START, SL_ROUTING_INFO,      1);
    REGIO_WRITE_FIELD_LITE (cmdbuf->rendec_block_start[RENDEC_SL_HDR], RENDEC_SLICE_INFO, SL_HDR_CK_START, SL_ENCODING_METHOD,   3);
    REGIO_WRITE_FIELD_LITE (cmdbuf->rendec_block_start[RENDEC_SL_HDR], RENDEC_SLICE_INFO, SL_HDR_CK_START, SL_NUM_SYMBOLS_LESS1, 1);

    cmdbuf->rendec_block_start[RENDEC_SL_NULL] = 0; /* empty */

    cmdbuf->cmd_idx += RENDEC_CK_HDR;
}    

/*
 * Start a new chunk in a RENDEC command block
 */
void psb_cmdbuf_rendec_start_chunk( psb_cmdbuf_p cmdbuf, uint32_t dest_address )
{
    ASSERT(NULL != cmdbuf->rendec_block_start); /* Must have a RENDEC block open */
    cmdbuf->rendec_chunk_start = cmdbuf->cmd_idx++;

    *cmdbuf->rendec_chunk_start = 0;
    REGIO_WRITE_FIELD_LITE(*cmdbuf->rendec_chunk_start, RENDEC_SLICE_INFO, CK_HDR, CK_ENCODING_METHOD, 3);
    REGIO_WRITE_FIELD_LITE(*cmdbuf->rendec_chunk_start, RENDEC_SLICE_INFO, CK_HDR, CK_START_ADDRESS, ( dest_address >> 2));
}    

void psb_cmdbuf_rendec_write_block( psb_cmdbuf_p cmdbuf, 
                                    unsigned char *block,
                                    uint32_t size )
{
    ASSERT((size & 0x3) == 0);
    int i;
    for( i = 0; i < size; i += 4)
    {
        uint32_t val = block[i] | (block[i+1] << 8) | (block[i+2] << 16) | (block[i+3] << 24);
        psb_cmdbuf_rendec_write( cmdbuf, val );
    }
}

void psb_cmdbuf_rendec_write_address( psb_cmdbuf_p cmdbuf, 
                                      psb_buffer_p buffer,
                                      uint32_t buffer_offset )
{
    RELOC(*cmdbuf->cmd_idx++, buffer_offset, buffer);
}

/*
 * Finish a RENDEC chunk
 */
void psb_cmdbuf_rendec_end_chunk( psb_cmdbuf_p cmdbuf )
{
    ASSERT(NULL != cmdbuf->rendec_block_start); /* Must have an open RENDEC block */
    ASSERT(NULL != cmdbuf->rendec_chunk_start); /* Must have an open RENDEC chunk */
    uint32_t dword_count = (cmdbuf->cmd_idx - cmdbuf->rendec_chunk_start) - 1;

    REGIO_WRITE_FIELD_LITE (*cmdbuf->rendec_chunk_start,
                RENDEC_SLICE_INFO,
                CK_HDR,
                CK_NUM_SYMBOLS_LESS1,
                (2 * dword_count) - 1);        /* Number of 16-bit symbols, minus 1.*/

    cmdbuf->rendec_chunk_start = NULL;
}

/*
 * Finish a RENDEC block
 */
void psb_cmdbuf_rendec_end_block( psb_cmdbuf_p cmdbuf )
{
    ASSERT(NULL != cmdbuf->rendec_block_start); /* Must have an open RENDEC block */
    ASSERT(NULL == cmdbuf->rendec_chunk_start); /* All chunks must be closed */

    uint32_t block_size = cmdbuf->cmd_idx - cmdbuf->rendec_block_start;  /* Include separator but not mtx block header*/

    /* Write separator (footer-type thing)    */    
    *cmdbuf->cmd_idx = 0;
    REGIO_WRITE_FIELD( *cmdbuf->cmd_idx, RENDEC_SLICE_INFO, SLICE_SEPARATOR, SL_SEP_SUFFIX, 7); 
    cmdbuf->cmd_idx++;

    /* Write CMD Header    */
    cmdbuf->rendec_block_start[MTX_CTRL_HEADER] = CMD_RENDEC_WRITE | block_size;

    cmdbuf->rendec_block_start = NULL;
}

/*
 * Returns the number of words left in the current segment
 */
uint32_t psb_cmdbuf_segment_space( psb_cmdbuf_p cmdbuf )
{
    uint32_t bytes_used = (void *) cmdbuf->cmd_idx - cmdbuf->cmd_start;
    return (MTX_SEG_SIZE - (bytes_used % MTX_SEG_SIZE)) / sizeof(uint32_t);
}

/*
 * Forwards the command buffer index to the next segment
 */
void psb_cmdbuf_next_segment( psb_cmdbuf_p cmdbuf )
{
    uint32_t *next_segment_cmd = cmdbuf->cmd_idx;
    cmdbuf->cmd_idx += 2;
    uint32_t words_free = psb_cmdbuf_segment_space( cmdbuf );
    
    if (cmdbuf->last_next_segment_cmd)
    {
        psb_cmdbuf_close_segment( cmdbuf );
    }
    else
    {
        cmdbuf->first_segment_size = (void *) cmdbuf->cmd_idx - cmdbuf->cmd_start;
    }

    cmdbuf->cmd_idx += words_free; /* move pui32CmdBuffer to start of next segment */
    
    cmdbuf->last_next_segment_cmd = next_segment_cmd;
}

/*
 * Create a conditional SKIP block
 */
void psb_cmdbuf_skip_start_block( psb_cmdbuf_p cmdbuf, uint32_t skip_condition )
{
    ASSERT(NULL == cmdbuf->rendec_block_start); /* Can't be inside a rendec block */
    ASSERT(NULL == cmdbuf->reg_start); /* Can't be inside a reg block */
    ASSERT(NULL == cmdbuf->skip_block_start); /* Can't be inside another skip block (limitation of current sw design)*/
    
    cmdbuf->skip_condition = skip_condition;
    cmdbuf->skip_block_start = cmdbuf->cmd_idx++;
}

/*
 * Terminate a conditional SKIP block
 */
void psb_cmdbuf_skip_end_block( psb_cmdbuf_p cmdbuf )
{
    ASSERT(NULL == cmdbuf->rendec_block_start); /* Rendec block must be closed */
    ASSERT(NULL == cmdbuf->reg_start); /* Reg block must be closed */
    ASSERT(NULL != cmdbuf->skip_block_start); /* Skip block must still be open */

    uint32_t block_size = cmdbuf->cmd_idx - (cmdbuf->skip_block_start + 1);

    *cmdbuf->skip_block_start = CMD_CONDITIONAL_SKIP | (cmdbuf->skip_condition << 20 ) | block_size;
    cmdbuf->skip_block_start = NULL;
}
