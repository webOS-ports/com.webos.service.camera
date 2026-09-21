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

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

static void ensureGstInit()
{
    /* gst_init() has no internal guard, and this is reachable from the LS2
     * handler thread and startCapture's build worker at the same time; a plain
     * bool would let two threads initialize concurrently. */
    static std::once_flag once;
    std::call_once(once, []() { gst_init(nullptr, nullptr); });
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
      buffers_(nullptr), nBuffers_(0), nextBuffer_(0), truncateWarned_(false)
{
    std::memset(&format_, 0, sizeof(format_));
    format_.pixel_format  = CAMERA_PIXEL_FORMAT_NV21;
    format_.stream_width  = 1280;
    format_.stream_height = 720;
    format_.stream_fps    = 30;
    format_.buffer_size   = format_.stream_width * format_.stream_height * 3 / 2;
}

DroidCameraPlugin::~DroidCameraPlugin() { teardownPipeline(); }

/* Device nodes are announced by the droid notifier as droid:<n>. Parse the
 * suffix strictly: atoi silently turns a mangled node into camera 0, which
 * would open the wrong sensor instead of failing the command. */
static bool parseDroidDeviceNode(const std::string &devname, int &device)
{
    const auto pos = devname.rfind(':');
    if (pos == std::string::npos || pos + 1 >= devname.size())
        return false;

    const char *suffix = devname.c_str() + pos + 1;
    char *end          = nullptr;
    errno              = 0;
    const long v       = strtol(suffix, &end, 10);
    if (errno != 0 || end == suffix || *end != '\0' || v < 0 || v > INT_MAX)
        return false;

    device = static_cast<int>(v);
    return true;
}

int DroidCameraPlugin::openDevice(std::string devname, std::string payload)
{
    PLOGI("devname: %s", devname.c_str());

    int device = 0;
    if (!parseDroidDeviceNode(devname, device))
    {
        PLOGE("not a droid device node: %s", devname.c_str());
        return CAMERA_ERROR_UNKNOWN;
    }
    cameraDevice_ = device;

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

    bool wasStreaming = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        wasStreaming = streaming_;
    }

    if (wasStreaming)
    {
        teardownPipeline();
        /* This runs outside startCapture's retry loop, so the rebuild gets the
         * whole 10 s budget to itself (see startCapture for where that number
         * comes from). */
        if (!buildPipeline(std::chrono::steady_clock::now() + std::chrono::seconds(10)))
            return CAMERA_ERROR_UNKNOWN;

        /* teardownPipeline cleared streaming_; without turning it back on the
         * rebuilt pipeline is live but every later getBuffer refuses to pull
         * from it and the preview dies. */
        std::lock_guard<std::mutex> guard(lock_);
        streaming_ = true;
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
        slotLengths_.clear();
        PLOGE("no user buffers supplied by the service (io_mode %d)", io_mode);
        return CAMERA_ERROR_UNKNOWN;
    }

    /* Snapshot each slot's capacity now: after every frame the service
     * overwrites slot->length with that frame's byte count, so the live field
     * stops describing how much the slot can hold once the first frame lands. */
    slotLengths_.assign(static_cast<size_t>(nBuffers_), 0);
    for (int i = 0; i < nBuffers_; i++)
        slotLengths_[i] = buffers_[i].length;
    truncateWarned_ = false;

    return CAMERA_ERROR_NONE;
}

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
static void stopPipelineBounded(GstElement *pipeline)
{
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

bool DroidCameraPlugin::buildPipeline(std::chrono::steady_clock::time_point deadline)
{
    gchar *desc = g_strdup_printf(
        "droidcamsrc name=droidcam camera-device=%d "
        "droidcam.imgsrc ! fakesink async=false "
        "droidcam.vidsrc ! fakesink async=false "
        "droidcam.vfsrc ! capsfilter caps=video/x-raw,format=NV21,width=%u,height=%u ! "
        "queue max-size-buffers=4 leaky=downstream ! "
        "appsink name=sink emit-signals=false sync=false max-buffers=2 drop=true",
        cameraDevice_, format_.stream_width, format_.stream_height);

    /* Build into locals and publish only a fully working pipeline at the end:
     * getBuffer and teardownPipeline read pipeline_/appsink_ under lock_, so
     * writing the members here while the state change is still in flight would
     * hand them a half-built pipeline. */
    GError *error        = nullptr;
    GstElement *pipeline = gst_parse_launch(desc, &error);
    g_free(desc);

    if (!pipeline)
    {
        PLOGE("pipeline: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return false;
    }
    g_clear_error(&error);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!sink)
    {
        /* gst_parse_launch can succeed while still not producing the element
         * we asked for by name; without the appsink there is nothing to pull
         * frames from, so a "working" pipeline here would only fail later in
         * getBuffer with no hint why. */
        PLOGE("pipeline has no appsink named \"sink\"");
        stopPipelineBounded(pipeline);
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* A failed state change on its own tells us nothing: the reason lives on
     * the pipeline bus, and droidcamsrc puts a real message there (no such
     * camera, HAL busy, droidmedia not reachable, ...). Without this the only
     * evidence is "cannot start droid pipeline", which is not enough to act on. */
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        logBusError(pipeline, "set_state(PLAYING) failed");
        gst_object_unref(sink);
        stopPipelineBounded(pipeline);
        return false;
    }

    /* PLAYING is reached asynchronously; a source that cannot open its device
     * usually returns ASYNC here and only fails once it tries. Wait for the
     * transition to settle so the failure is caught now rather than surfacing
     * later as an empty appsink. */
    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        GstState state = GST_STATE_NULL;
        /* Wait only as long as the caller's deadline allows: the deadline is
         * derived from the camera service's own budget (CameraHalProxy gives
         * startPreview COMMAND_TIMEOUT_LONG, 12 s, part of which the
         * shared-memory setup and setBuffer have already spent), and a retrying
         * caller has already burned part of it on earlier attempts. droidcamsrc
         * prerolls in a couple of seconds when it works, so whatever is left is
         * enough to catch a source that cannot open its device. */
        const auto remaining = deadline - std::chrono::steady_clock::now();
        GstClockTime wait    = 0;
        if (remaining > std::chrono::steady_clock::duration::zero())
            wait = static_cast<GstClockTime>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count());
        ret = gst_element_get_state(pipeline, &state, nullptr, wait);
        if (ret != GST_STATE_CHANGE_SUCCESS || state != GST_STATE_PLAYING)
        {
            logBusError(pipeline, "pipeline did not reach PLAYING");
            gst_object_unref(sink);
            stopPipelineBounded(pipeline);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> guard(lock_);
        pipeline_ = pipeline;
        appsink_  = sink;
    }
    return true;
}

/* Drain whatever the pipeline bus is holding and log it. Called only on the
 * failure paths, so the ordinary case stays quiet. */
void DroidCameraPlugin::logBusError(GstElement *pipeline, const char *context)
{
    PLOGE("%s", context);

    if (!pipeline)
        return;

    GstBus *bus = gst_element_get_bus(pipeline);
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

    stopPipelineBounded(pipeline);
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
     * Retry the whole build a couple of times, but against one shared
     * deadline: the camera service gives startPreview COMMAND_TIMEOUT_LONG
     * (12 s) and per-attempt costs stack up - the PLAYING wait, a teardown
     * that may burn its full 3 s bound, the pause between attempts - so fixed
     * per-attempt timeouts can overshoot it. Budget 10 s here to leave the
     * service room to answer its own caller, hand each attempt whatever is
     * left, and stop retrying once another attempt cannot fit. */
    constexpr int kAttempts = 3;
    constexpr auto kRetryPause = std::chrono::milliseconds(300);
    /* droidcamsrc prerolls in a couple of seconds when it works; an attempt
     * with less time than this left cannot succeed and only delays the error. */
    constexpr auto kMinAttemptBudget = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

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
            std::thread worker([this, deadline, &built]()
                               { built = buildPipeline(deadline); });
            worker.join();
        }

        if (built)
        {
            if (attempt > 1)
                PLOGI("droid pipeline started on attempt %d", attempt);
            std::lock_guard<std::mutex> guard(lock_);
            streaming_ = true;
            return CAMERA_ERROR_NONE;
        }

        PLOGW("droid pipeline attempt %d/%d failed", attempt, kAttempts);
        if (attempt == kAttempts)
            break;
        if (std::chrono::steady_clock::now() + kRetryPause + kMinAttemptBudget >= deadline)
        {
            PLOGW("no budget left for attempt %d/%d, giving up", attempt + 1, kAttempts);
            break;
        }
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
    /* Capacity comes from the setBuffer-time snapshot, not slot->length: the
     * service overwrites that field with each frame's byte count, so from the
     * second frame on it describes the previous frame, not the slot. */
    const size_t capacity = (static_cast<size_t>(nextBuffer_) < slotLengths_.size())
                                ? slotLengths_[nextBuffer_]
                                : 0;
    if (!slot->start || capacity == 0)
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
        if (n > capacity)
        {
            /* A frame that does not fit means the negotiated caps and the
             * shm layout disagree; the consumer gets a truncated frame, so
             * say so - once, this repeats every frame. */
            if (!truncateWarned_)
            {
                PLOGW("frame of %" G_GSIZE_FORMAT " bytes exceeds slot capacity "
                      "%zu; truncating", map.size, capacity);
                truncateWarned_ = true;
            }
            n = capacity;
        }
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
    slotLengths_.clear();
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

    int dev = 0;
    if (!parseDroidDeviceNode(devicenode, dev))
    {
        PLOGE("not a droid device node: %s", devicenode.c_str());
        return CAMERA_ERROR_UNKNOWN;
    }

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
