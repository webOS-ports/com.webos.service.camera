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

#define LOG_CONTEXT "hal.droid"
#define LOG_TAG "DroidCameraPlugin"
#include "droid_camera_plugin.h"
#include "camera_log.h"
#include "plugin.hpp"

#include <cstring>

static void ensureGstInit()
{
    static bool done = false;
    if (!done)
    {
        gst_init(nullptr, nullptr);
        done = true;
    }
}

/* Ask the GStreamer registry instead of probing a fixed path. gst-droid is not
 * always in the directory GStreamer scans by default: LuneOS installs it in
 * ${libdir}/gstreamer-1.0-gated and gst-droid-gate.service adds that to
 * GST_PLUGIN_PATH once the Android media services answer, so a hardcoded
 * ${libdir}/gstreamer-1.0 probe reports "not installed" on exactly the devices
 * where it is installed and working. */
static bool droidPluginAvailable()
{
    ensureGstInit();

    GstElementFactory *factory = gst_element_factory_find("droidcamsrc");
    if (!factory)
        return false;

    gst_object_unref(factory);
    return true;
}

/* The camera-device GParamSpec's maximum is clamped to the real camera count
 * minus one once the element has initialized its HAL connection. */
int DroidCameraPlugin::deviceCount()
{
    ensureGstInit();

    /* gst_element_factory_make() already returns NULL when gst-droid is not
     * installed, so no separate availability probe is needed here. */
    GstElement *e = gst_element_factory_make("droidcamsrc", nullptr);
    if (!e)
        return 0;

    int count = 0;
    if (gst_element_set_state(e, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE)
    {
        gst_element_get_state(e, nullptr, nullptr, 5 * GST_SECOND);
        GParamSpec *spec =
            g_object_class_find_property(G_OBJECT_GET_CLASS(e), "camera-device");
        if (spec && G_IS_PARAM_SPEC_INT(spec))
            count = G_PARAM_SPEC_INT(spec)->maximum + 1;
    }
    gst_element_set_state(e, GST_STATE_NULL);
    gst_object_unref(e);

    PLOGI("droid camera count: %d", count);
    return count;
}

DroidCameraPlugin::DroidCameraPlugin()
    : cameraDevice_(0), pipeline_(nullptr), appsink_(nullptr), streaming_(false),
      buffers_(nullptr), nBuffers_(0), nextBuffer_(0)
{
    std::memset(&format_, 0, sizeof(format_));
    format_.pixel_format  = CAMERA_PIXEL_FORMAT_NV21;
    format_.stream_width  = 1280;
    format_.stream_height = 720;
    format_.stream_fps    = 30;
    format_.buffer_size   = format_.stream_width * format_.stream_height * 3 / 2;
}

DroidCameraPlugin::~DroidCameraPlugin() { teardownPipeline(); }

int DroidCameraPlugin::openDevice(std::string devname, std::string payload)
{
    PLOGI("devname: %s", devname.c_str());

    /* device nodes are announced by the droid notifier as droid:<n> */
    const auto pos = devname.rfind(':');
    cameraDevice_  = (pos != std::string::npos) ? atoi(devname.c_str() + pos + 1) : 0;

    if (!droidPluginAvailable())
    {
        PLOGE("gst-droid not available");
        return CAMERA_ERROR_UNKNOWN;
    }

    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::closeDevice()
{
    teardownPipeline();
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::setFormat(const void *stream_format)
{
    const stream_format_t *in = static_cast<const stream_format_t *>(stream_format);

    PLOGI("requested %ux%u@%d fmt %d", in->stream_width, in->stream_height,
          in->stream_fps, in->pixel_format);

    /* Raw preview from droidcamsrc is NV21; the service's consumers get the
     * effective format back from getFormat. */
    format_               = *in;
    format_.pixel_format  = CAMERA_PIXEL_FORMAT_NV21;
    if (format_.stream_fps <= 0)
        format_.stream_fps = 30;
    format_.buffer_size =
        format_.stream_width * format_.stream_height * 3 / 2;

    if (streaming_)
    {
        teardownPipeline();
        if (!buildPipeline())
            return CAMERA_ERROR_UNKNOWN;
    }
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::getFormat(void *stream_format)
{
    *static_cast<stream_format_t *>(stream_format) = format_;
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::setBuffer(int num_buffer, int io_mode, void **usrbufs)
{
    /* IOMODE_USERPTR: the service has already created its shared-memory frame
     * slots and fills shmDataBuffers[i].start from the shm data list before
     * calling us, then passes that array here. Keep it - getBuffer copies each
     * frame into one of these slots and hands the slot back. */
    std::lock_guard<std::mutex> guard(lock_);

    buffers_    = usrbufs ? *reinterpret_cast<buffer_t **>(usrbufs) : nullptr;
    nBuffers_   = (num_buffer > 0) ? num_buffer : 0;
    nextBuffer_ = 0;

    if (!buffers_ || nBuffers_ == 0)
    {
        PLOGE("no user buffers supplied by the service (io_mode %d)", io_mode);
        return CAMERA_ERROR_UNKNOWN;
    }
    return CAMERA_ERROR_NONE;
}

bool DroidCameraPlugin::buildPipeline()
{
    gchar *desc = g_strdup_printf(
        "droidcamsrc name=droidcam camera-device=%d "
        "droidcam.imgsrc ! fakesink async=false "
        "droidcam.vidsrc ! fakesink async=false "
        "droidcam.vfsrc ! capsfilter caps=video/x-raw,format=NV21,width=%u,height=%u ! "
        "queue max-size-buffers=4 leaky=downstream ! "
        "appsink name=sink emit-signals=false sync=false max-buffers=2 drop=true",
        cameraDevice_, format_.stream_width, format_.stream_height);

    GError *error = nullptr;
    pipeline_     = gst_parse_launch(desc, &error);
    g_free(desc);

    if (!pipeline_)
    {
        PLOGE("pipeline: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return false;
    }
    g_clear_error(&error);

    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
    {
        PLOGE("cannot start droid pipeline");
        teardownPipeline();
        return false;
    }
    return true;
}

void DroidCameraPlugin::teardownPipeline()
{
    std::lock_guard<std::mutex> guard(lock_);
    if (pipeline_)
    {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (appsink_)
        {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    streaming_ = false;
}

int DroidCameraPlugin::startCapture()
{
    ensureGstInit();
    if (!buildPipeline())
        return CAMERA_ERROR_UNKNOWN;
    streaming_ = true;
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::stopCapture()
{
    teardownPipeline();
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::getBuffer(void *outbuf)
{
    buffer_t *buf = static_cast<buffer_t *>(outbuf);

    std::lock_guard<std::mutex> guard(lock_);

    /* The service hands in a zeroed buffer_t and expects the plugin to fill
     * start/length/index in, exactly as V4l2CameraPlugin::getBuffer does:
     * buf->start is an output, not a destination provided by the caller. The
     * frame therefore goes into one of the slots kept from setBuffer. */
    if (!appsink_ || !streaming_ || !buf || !buffers_ || nBuffers_ == 0)
        return CAMERA_ERROR_UNKNOWN;

    buffer_t *slot = &buffers_[nextBuffer_];
    if (!slot->start || slot->length == 0)
    {
        PLOGE("user buffer %d is not usable", nextBuffer_);
        return CAMERA_ERROR_UNKNOWN;
    }

    GstSample *sample = nullptr;
    /* try-pull-sample is an action signal, so no gstapp headers/library are
     * needed at build time. */
    g_signal_emit_by_name(appsink_, "try-pull-sample", 2 * GST_SECOND, &sample);
    if (!sample)
    {
        PLOGE("no frame from droidcamsrc");
        return CAMERA_ERROR_UNKNOWN;
    }

    GstBuffer *gbuf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    int ret = CAMERA_ERROR_UNKNOWN;
    if (gbuf && gst_buffer_map(gbuf, &map, GST_MAP_READ))
    {
        /* never write past the shared-memory slot the service allocated */
        gsize n = map.size;
        if (n > slot->length)
            n = slot->length;
        std::memcpy(slot->start, map.data, n);

        buf->start  = slot->start;
        buf->length = n;
        buf->index  = static_cast<size_t>(nextBuffer_);

        nextBuffer_ = (nextBuffer_ + 1) % nBuffers_;

        gst_buffer_unmap(gbuf, &map);
        ret = CAMERA_ERROR_NONE;
    }
    gst_sample_unref(sample);
    return ret;
}

/* Nothing to hand back: the frame was copied into the service's own
 * shared-memory slot in getBuffer and the GStreamer sample dropped there and
 * then, so no droid-side buffer is held across the call. */
int DroidCameraPlugin::releaseBuffer(const void *) { return CAMERA_ERROR_NONE; }

int DroidCameraPlugin::destroyBuffer()
{
    /* The slots belong to the service, which frees them after this returns;
     * stop pointing at them so a late getBuffer cannot touch freed memory. */
    std::lock_guard<std::mutex> guard(lock_);
    buffers_    = nullptr;
    nBuffers_   = 0;
    nextBuffer_ = 0;
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::setProperties(const void *) { return CAMERA_ERROR_NONE; }

int DroidCameraPlugin::getProperties(void *cam_out_param)
{
    camera_properties_t *out = static_cast<camera_properties_t *>(cam_out_param);
    for (auto &row : out->stGetData.data)
        for (auto &v : row)
            v = -999; /* CONST_PARAM_DEFAULT_VALUE: not supported */
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::getInfo(void *cam_info, std::string devicenode)
{
    camera_device_info_t *info = static_cast<camera_device_info_t *>(cam_info);

    const auto pos = devicenode.rfind(':');
    const int dev  = (pos != std::string::npos) ? atoi(devicenode.c_str() + pos + 1) : 0;

    info->str_devicename = (dev == 0) ? "Droid back camera" : "Droid front camera";
    info->str_vendorid   = "droid";
    info->str_productid  = std::to_string(dev);
    info->n_devicetype   = DEVICE_TYPE_CAMERA;
    info->b_builtin      = 1;
    info->stResolution.clear();
    info->stResolution.emplace_back(
        std::vector<std::string>{"1920,1080,30", "1280,720,30", "640,480,30"},
        CAMERA_FORMAT_YUV);
    return CAMERA_ERROR_NONE;
}

int DroidCameraPlugin::getBufferFd(int *, int *) { return CAMERA_ERROR_UNKNOWN; }

extern "C"
{
    IPlugin *plugin_init(void)
    {
        Plugin *plg = new Plugin();
        plg->setName("Droid Hal");
        plg->setDescription("gst-droid Camera HAL for Halium devices");
        plg->setCategory("HAL");
        plg->setVersion("1.0.0");
        plg->setOrganization("LuneOS project");
        plg->registerFeature<DroidCameraPlugin>("droid");

        return plg;
    }

    void __attribute__((constructor)) plugin_load(void)
    {
        printf("%s:%s\n", __FILENAME__, __PRETTY_FUNCTION__);
    }

    void __attribute__((destructor)) plugin_unload(void)
    {
        printf("%s:%s\n", __FILENAME__, __PRETTY_FUNCTION__);
    }
}
