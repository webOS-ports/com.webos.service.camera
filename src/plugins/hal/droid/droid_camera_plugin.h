// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef DROID_CAMERA_PLUGIN_H_
#define DROID_CAMERA_PLUGIN_H_

#include "camera_hal_types.h"
#include "camera_hal_types_common.h"
#include "plugin_interface.hpp"

#include <gst/gst.h>
#include <mutex>
#include <string>

/*
 * Camera HAL plugin for Halium devices: the cameras sit behind the Android
 * camera HAL and are reached through gst-droid's droidcamsrc element (via
 * the droidmedia services in the Android container). Frames are delivered
 * to the camera service as raw NV21 system-memory buffers pulled from an
 * appsink, which the service copies into its shared-memory distribution.
 */
class DroidCameraPlugin : public IHal
{
public:
    DroidCameraPlugin();
    virtual ~DroidCameraPlugin();

    virtual bool queryInterface(const char *szName, void **ppInterface) override
    {
        *ppInterface = static_cast<void *>(static_cast<IHal *>(this));
        return true;
    }

    virtual int openDevice(std::string devname, std::string payload) override;
    virtual int closeDevice() override;
    virtual int setFormat(const void *stream_format) override;
    virtual int getFormat(void *stream_format) override;
    virtual int setBuffer(int num_buffer, int io_mode, void **usrbufs) override;
    virtual int getBuffer(void *outbuf) override;
    virtual int releaseBuffer(const void *inbuf) override;
    virtual int destroyBuffer() override;
    virtual int startCapture() override;
    virtual int stopCapture() override;
    virtual int setProperties(const void *cam_in_param) override;
    virtual int getProperties(void *cam_out_param) override;
    virtual int getInfo(void *cam_info, std::string devicenode) override;
    virtual int getBufferFd(int *bufFd, int *count) override;

    static int deviceCount();

private:
    bool buildPipeline();
    void logBusError(const char *context);
    void teardownPipeline();

    int cameraDevice_;
    stream_format_t format_;
    GstElement *pipeline_;
    GstElement *appsink_;
    bool streaming_;

    /* The shared-memory frame slots the service allocates and hands over in
     * setBuffer (IOMODE_USERPTR). Frames are copied into these and the slot
     * is returned to the service, as V4l2CameraPlugin does for USERPTR. */
    buffer_t *buffers_;
    int nBuffers_;
    int nextBuffer_;
    // serializes getBuffer against teardown: the service's preview thread
    // pulls frames while stopCapture may run from the LS2 thread
    std::mutex lock_;
};

#endif /* DROID_CAMERA_PLUGIN_H_ */
