/*
 * Copyright (c) 2011 Intel Corporation. All Rights Reserved.
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
 *
 * Authors:
 *    tianyang.zhu <tianyang.zhu@intel.com>
 */

//#define LOG_NDEBUG 0

#define LOG_TAG "MultiDisplay"

#include <utils/Log.h>
#include "psb_mds.h"

namespace android {
namespace intel {

psbMultiDisplayListener::psbMultiDisplayListener() {
#ifndef USE_MDS_LEGACY
    // get mds service and register listener
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        LOGE("%s: Fail to get service manager", __func__);
        return;
    }
    mMds = interface_cast<IMDService>(sm->getService(String16(INTEL_MDS_SERVICE_NAME)));
    if (mMds == NULL) {
        LOGE("%s: Failed to get Mds service", __func__);
        return;
    }
    mListener = mMds->getInfoProvider();
#else
    mListener = new MultiDisplayClient();
    if (mListener == NULL)
        return;
#endif
    return;
}

psbMultiDisplayListener::~psbMultiDisplayListener() {
#ifdef USE_MDS_LEGACY
    if (mListener != NULL)
        delete mListener;
#endif
    mListener = NULL;
    return;
}

int psbMultiDisplayListener::getMode() {
    int mode = MDS_MODE_NONE;
    if (mListener == NULL) return MDS_MODE_NONE;
#ifndef USE_MDS_LEGACY
    if (mListener.get() == NULL) return MDS_INIT_VALUE;
#endif
    mode = mListener->getDisplayMode(false);

#ifndef USE_MDS_LEGACY
    if (checkMode(mode, (MDS_VIDEO_ON | MDS_HDMI_CONNECTED)))
        mode = MDS_HDMI_VIDEO_ISPLAYING;
    else if (checkMode(mode, (MDS_VIDEO_ON | MDS_WIDI_ON)))
        mode = MDS_WIDI_VIDEO_ISPLAYING;
    else
        mode = MDS_INIT_VALUE;
#else
    if (checkMode(mode, MDS_HDMI_VIDEO_EXT))
        mode = MDS_HDMI_VIDEO_ISPLAYING;
    else if (checkMode(mode,MDS_WIDI_ON))
        mode = MDS_WIDI_VIDEO_ISPLAYING;
    else
        mode = MDS_INIT_VALUE;
#endif
    //ALOGV("mds mode is %d", mode);
    return mode;
}

bool psbMultiDisplayListener::getDecoderOutputResolution(
        int32_t* width, int32_t* height,
        int32_t* offX, int32_t* offY,
        int32_t* bufW, int32_t* bufH) {
#ifndef USE_MDS_LEGACY
    if (mListener.get() == NULL ||
            width == NULL || height == NULL ||
            offX == NULL || offY == NULL ||
            bufW == NULL || bufH == NULL)
        return false;
    // only for WIDI video playback,
    // TODO: HWC doesn't set the bit "MDS_WIDI_ON" rightly now
    int mode = mListener->getDisplayMode(false);
    if (!checkMode(mode, (MDS_VIDEO_ON | MDS_WIDI_ON)))
        return false;
    status_t ret = mListener->getDecoderOutputResolution(0, width, height, offX, offY, bufW, bufH);
    return (ret == NO_ERROR);
#else
    return false;
#endif
}

bool psbMultiDisplayListener::getVppState() {
    return false;
#ifndef USE_MDS_LEGACY
    if (mListener.get() == NULL) {
        ALOGE("MDS listener is null");
        return false;
    }
    return mListener->getVppState();
#else
    return false;
#endif
}

}; // namespace intel
}; // namespace android
