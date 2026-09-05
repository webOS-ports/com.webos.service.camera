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

#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

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

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);

    /* A failed state change on its own tells us nothing: the reason lives on
     * the pipeline bus, and droidcamsrc puts a real message there (no such
     * camera, HAL busy, droidmedia not reachable, ...). Without this the only
     * evidence is "cannot start droid pipeline", which is not enough to act on. */
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        logBusError("set_state(PLAYING) failed");
        teardownPipeline();
        return false;
    }

    /* PLAYING is reached asynchronously; a source that cannot open its device
     * usually returns ASYNC here and only fails once it tries. Wait for the
     * transition to settle so the failure is caught now rather than surfacing
     * later as an empty appsink. */
    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        GstState state = GST_STATE_NULL;
        /* Must fit inside the camera service's own budget: CameraHalProxy
         * gives startPreview COMMAND_TIMEOUT_LONG (12 s), and the shared-memory
         * setup and setBuffer have already spent part of it. Long enough to
         * catch a source that cannot open its device - droidcamsrc prerolls in
         * a couple of seconds when it works - while leaving the service room
         * to answer its own caller. */
        ret = gst_element_get_state(pipeline_, &state, nullptr, 5 * GST_SECOND);
        if (ret != GST_STATE_CHANGE_SUCCESS || state != GST_STATE_PLAYING)
        {
            logBusError("pipeline did not reach PLAYING");
            teardownPipeline();
            return false;
        }
    }
    return true;
}

/* Drain whatever the pipeline bus is holding and log it. Called only on the
 * failure paths, so the ordinary case stays quiet. */
void DroidCameraPlugin::logBusError(const char *context)
{
    PLOGE("%s", context);

    if (!pipeline_)
        return;

    GstBus *bus = gst_element_get_bus(pipeline_);
    if (!bus)
        return;

    while (GstMessage *msg = gst_bus_pop_filtered(
               bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)))
    {
        GError *err = nullptr;
        gchar *dbg  = nullptr;

        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            gst_message_parse_error(msg, &err, &dbg);
        else
            gst_message_parse_warning(msg, &err, &dbg);

        PLOGE("  %s from %s: %s (%s)",
              GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ? "error" : "warning",
              GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)), err ? err->message : "?",
              dbg ? dbg : "no detail");

        g_clear_error(&err);
        g_free(dbg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

void DroidCameraPlugin::teardownPipeline()
{
    GstElement *pipeline = nullptr;
    GstElement *sink     = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        pipeline   = pipeline_;
        sink       = appsink_;
        pipeline_  = nullptr;
        appsink_   = nullptr;
        streaming_ = false;
    }

    if (sink)
        gst_object_unref(sink);
    if (!pipeline)
        return;

    /* The NULL transition is the dangerous one. When droidcamsrc is wedged
     * part-way through bringing the vendor camera up, gst_element_set_state()
     * does not come back, and because this runs in the short-lived
     * com.webos.service.camera2.hal child that hangs the whole process. The
     * Android camera client is then never released - logcat shows the
     * "Camera N: Opened" with no matching disconnect - and every later attempt,
     * including a plain gst-launch, fails until the device is rebooted.
     *
     * So bound it. If the pipeline will not stop, abandon it and let the
     * process exit: exiting drops the binder connection, which is what actually
     * makes CameraService release the camera. Leaking a pipeline in a process
     * that is about to die costs nothing; hanging it costs a reboot. */
    auto finished = std::make_shared<std::promise<void>>();
    std::future<void> stopped = finished->get_future();

    std::thread(
        [pipeline, finished]()
        {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            finished->set_value();
        })
        .detach();

    if (stopped.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
        PLOGE("droid pipeline did not reach NULL within 3s; abandoning it so the "
              "process can exit and release the camera");
}

int DroidCameraPlugin::startCapture()
{
    ensureGstInit();

    /* The vendor HAL frequently fails the first preview start with
     * "error 0x1 from camera HAL", and droidcamsrc reports "error starting
     * preview". That is not fatal: a plain gst-launch pipeline hits exactly the
     * same error on this hardware and still reaches PLAYING, because it keeps
     * going instead of tearing down. We were giving up on the first attempt.
     *
     * Retry the whole build a couple of times. Each failed attempt costs about
     * a second, and the budget has to stay inside the camera service's
     * COMMAND_TIMEOUT_LONG (12 s) for startPreview. */
    constexpr int kAttempts = 3;
    for (int attempt = 1; attempt <= kAttempts; attempt++)
    {
        /* Build on a thread of our own rather than on the luna-service2
         * handler thread we were called from. Bringing droidcamsrc to PLAYING
         * blocks, and doing that on the service's main loop stops it dispatching
         * anything else - including whatever droidmedia may be waiting on. A
         * plain gst-launch reaches PLAYING with the identical pipeline, and the
         * calling context is the most visible thing that differs. */
        bool built = false;
        {
            std::thread worker([this, &built]() { built = buildPipeline(); });
            worker.join();
        }

        if (built)
        {
            if (attempt > 1)
                PLOGI("droid pipeline started on attempt %d", attempt);
            streaming_ = true;
            return CAMERA_ERROR_NONE;
        }

        PLOGW("droid pipeline attempt %d/%d failed", attempt, kAttempts);
        if (attempt < kAttempts)
            g_usleep(300 * 1000);
    }

    return CAMERA_ERROR_UNKNOWN;
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
