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
#include <string>
#include <vector>

/*
 * Announces the built-in Halium (Android HAL) cameras to the camera
 * service. The device count is probed through gst-droid; devices are
 * published once from an idle callback on the service main loop with
 * device nodes of the form droid:<n>, which the droid HAL plugin parses.
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

class DroidNotifier : public INotifier
{
public:
    DroidNotifier() {}
    virtual ~DroidNotifier()
    {
        if (idle_source_)
            g_source_destroy(idle_source_);
    }

    virtual bool queryInterface(const char *szName, void **ppInterface) override
    {
        *ppInterface = static_cast<void *>(static_cast<INotifier *>(this));
        return true;
    }

    virtual void subscribeToClient(handlercb cb, void *mainLoop) override
    {
        updateDeviceList_ = std::move(cb);

        GMainContext *ctx =
            mainLoop ? g_main_loop_get_context(static_cast<GMainLoop *>(mainLoop))
                     : nullptr;
        idle_source_ = g_idle_source_new();
        g_source_set_callback(
            idle_source_,
            +[](gpointer self) -> gboolean {
                static_cast<DroidNotifier *>(self)->announce();
                return G_SOURCE_REMOVE;
            },
            this, nullptr);
        g_source_attach(idle_source_, ctx);
    }

    virtual void setLSHandle(void *) override {}

private:
    void announce()
    {
        idle_source_ = nullptr;

        const int count = droid_camera_count();
        PLOGI("announcing %d droid camera(s)", count);

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

        if (updateDeviceList_)
            updateDeviceList_("droid", &devList);
    }

    handlercb updateDeviceList_;
    GSource *idle_source_ = nullptr;
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
