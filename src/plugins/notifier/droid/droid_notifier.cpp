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

#define LOG_CONTEXT "notifier.droid"
#define LOG_TAG "DroidNotifier"
#include "camera_device_types.h"
#include "camera_log.h"
#include "plugin.hpp"
#include "plugin_interface.hpp"

#include <glib.h>
#include <gst/gst.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

/*
 * Announces the built-in Halium (Android HAL) cameras to the camera
 * service. Devices are published with device nodes of the form droid:<n>,
 * which the droid HAL plugin parses.
 *
 * The probe runs on its own thread, never on the service main loop.
 * droid_camera_count() takes droidcamsrc to GST_STATE_READY, and that state
 * change opens the vendor camera HAL through droidmedia - a synchronous call
 * into the Android side that does not always return. Run from an idle callback
 * on the main loop, as this notifier originally did, a single stuck probe
 * wedges the whole camera service: getCameraList stops answering, and because
 * com.webos.service.camera2 talks to its camera2.hal child with synchronous
 * luna_call_sync, nothing recovers short of a reboot.
 *
 * The thread also retries, because the first probe after boot usually returns
 * zero: the notifier starts before droidmedia is ready inside the container,
 * and the camera service otherwise has to be restarted by hand before any
 * camera appears.
 */

/* The camera-device GParamSpec's maximum is clamped to the real camera
 * count minus one once droidcamsrc has initialized its HAL connection.
 * Availability is decided by gst_element_factory_make() returning NULL rather
 * than by probing a fixed path: LuneOS keeps gst-droid out of the directory
 * GStreamer scans by default and exposes it through GST_PLUGIN_PATH (see
 * gst-droid-gate.service), so a hardcoded path probe misses it. */
static int droid_camera_count()
{
    static bool gst_ready = false;
    if (!gst_ready)
    {
        gst_init(nullptr, nullptr);
        gst_ready = true;
    }

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
    return count;
}

namespace
{
/* Retry budget for the "container not ready yet" case. Six attempts two
 * seconds apart covers a slow droidmedia start without keeping a thread alive
 * for the whole session on a device that simply has no droid cameras. */
constexpr int kProbeAttempts = 6;
constexpr useconds_t kProbeRetryUs = 2 * 1000 * 1000;

/* Shared between the notifier and its probe thread. The thread can outlive the
 * notifier (a probe stuck in the HAL cannot be interrupted), so everything it
 * touches lives here behind a shared_ptr and it checks  before
 * calling back. */
struct ProbeState
{
    GMainContext *ctx = nullptr;
    INotifier::handlercb cb;
    std::atomic<bool> cancelled{false};
    int count = 0;
};

std::vector<DEVICE_LIST_T> BuildDeviceList(int count)
{
    std::vector<DEVICE_LIST_T> devList;
    for (int i = 0; i < count; i++)
    {
        DEVICE_LIST_T dev;
        dev.nDeviceNum       = i;
        dev.nPortNum         = 0;
        dev.isPowerOnConnect = 1;
        dev.strVendorName    = "LuneOS";
        dev.strProductName   = (i == 0) ? "Droid back camera" : "Droid front camera";
        dev.strVendorID      = "droid";
        dev.strProductID     = std::to_string(i);
        dev.strDeviceType    = "droid";
        dev.strDeviceSubtype = "builtin";
        dev.strDeviceNode    = "droid:" + std::to_string(i);
        dev.strDeviceKey     = dev.strDeviceNode;
        devList.push_back(dev);
    }
    return devList;
}

/* Runs on the service main loop, so the callback into the camera service
 * happens where it expects to be called. */
gboolean PublishOnMainLoop(gpointer data)
{
    std::unique_ptr<std::shared_ptr<ProbeState>> holder(
        static_cast<std::shared_ptr<ProbeState> *>(data));
    ProbeState *st = holder->get();

    if (!st->cancelled.load() && st->cb)
    {
        PLOGI("announcing %d droid camera(s)", st->count);
        std::vector<DEVICE_LIST_T> devList = BuildDeviceList(st->count);
        st->cb("droid", &devList);
    }
    return G_SOURCE_REMOVE;
}

gpointer ProbeThread(gpointer data)
{
    std::unique_ptr<std::shared_ptr<ProbeState>> holder(
        static_cast<std::shared_ptr<ProbeState> *>(data));
    std::shared_ptr<ProbeState> st = *holder;

    int count = 0;
    for (int attempt = 0; attempt < kProbeAttempts && !st->cancelled.load(); attempt++)
    {
        count = droid_camera_count();
        if (count > 0)
            break;
        PLOGI("no droid cameras yet (attempt %d/%d), retrying", attempt + 1, kProbeAttempts);
        usleep(kProbeRetryUs);
    }

    if (st->cancelled.load())
        return nullptr;

    st->count = count;

    GSource *idle = g_idle_source_new();
    g_source_set_callback(idle, PublishOnMainLoop,
                          new std::shared_ptr<ProbeState>(st), nullptr);
    g_source_attach(idle, st->ctx);
    g_source_unref(idle);
    return nullptr;
}
} // namespace

class DroidNotifier : public INotifier
{
public:
    DroidNotifier() {}
    virtual ~DroidNotifier()
    {
        /* The probe thread may still be blocked inside the HAL; it cannot be
         * joined safely, so tell it to drop its result instead. */
        if (state_)
            state_->cancelled = true;
    }

    virtual bool queryInterface(const char *szName, void **ppInterface) override
    {
        *ppInterface = static_cast<void *>(static_cast<INotifier *>(this));
        return true;
    }

    virtual void subscribeToClient(handlercb cb, void *mainLoop) override
    {
        state_      = std::make_shared<ProbeState>();
        state_->ctx = mainLoop ? g_main_loop_get_context(static_cast<GMainLoop *>(mainLoop))
                               : nullptr;
        state_->cb  = std::move(cb);

        GThread *t = g_thread_new("droid-cam-probe", ProbeThread,
                                  new std::shared_ptr<ProbeState>(state_));
        /* Detached: nothing waits on it, and it must not hold up shutdown. */
        g_thread_unref(t);
    }

    virtual void setLSHandle(void *) override {}

private:
    std::shared_ptr<ProbeState> state_;
};

extern "C"
{
    IPlugin *plugin_init(void)
    {
        Plugin *plg = new Plugin();
        plg->setName("Droid Notifier");
        plg->setDescription("Built-in Halium camera notifier");
        plg->setCategory("NOTIFIER");
        plg->setVersion("1.0.0");
        plg->setOrganization("LuneOS project");
        plg->registerFeature<DroidNotifier>("droid-notifier");

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
