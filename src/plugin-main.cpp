/*
This file is part of the atkAudio plugin for OBS.
It is distributed under the AGPLv3 license. See the LICENSE file for details.

VST3-only build: the only thing this module registers is the "VST 3.x Eklentisi" audio filter
(atkAudio PluginHost), which hosts one VST3 plugin per filter instance.
*/

#include "config.h"
#include "core/atkaudio/Logging.h"
#include "core/atkaudio/atkaudio.h"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR(PLUGIN_AUTHOR)

MODULE_EXPORT const char* obs_module_name(void)
{
    return PLUGIN_DISPLAY_NAME;
}

MODULE_EXPORT const char* obs_module_description(void)
{
    return "VST3 plugin host audio filter based on atkAudio PluginHost (" PLUGIN_BUILD_VARIANT ", no network access)";
}

extern struct obs_source_info pluginhost_filter;

namespace
{
bool g_lifecycleCreated = false;

obs_module_t* findLoadedModuleConflictByName()
{
    auto* currentModule = obs_current_module();
    auto* loadedModuleByName = obs_get_module(PLUGIN_NAME);

    if (loadedModuleByName == nullptr || loadedModuleByName == currentModule)
        return nullptr;

    return loadedModuleByName;
}
} // namespace

bool obs_module_load(void)
{
    if (auto* loadedModuleConflict = findLoadedModuleConflictByName())
    {
        const char* loadedBinaryPath = obs_get_module_binary_path(loadedModuleConflict);
        atk::logging::error(
            "OBS_API",
            juce::String("another ") + PLUGIN_NAME + " is already loaded from \""
                + (loadedBinaryPath ? loadedBinaryPath : "(unknown)")
                + "\"; keep only one installation. This copy is not loaded."
        );
        return false;
    }

    if (!atk::create())
    {
        atk::logging::error("OBS_API", "failed to initialize the JUCE runtime");
        return false;
    }

    g_lifecycleCreated = true;

    obs_register_source(&pluginhost_filter);

    blog(
        LOG_INFO,
        "[%s] plugin loaded successfully (version %s, %s, running on OBS %s)",
        PLUGIN_NAME,
        PLUGIN_VERSION,
        PLUGIN_BUILD_VARIANT,
        obs_get_version_string()
    );

    return true;
}

void obs_module_unload(void)
{
    if (g_lifecycleCreated)
    {
        atk::destroy();
        g_lifecycleCreated = false;
    }

    blog(LOG_INFO, "[%s] plugin unloaded", PLUGIN_NAME);
}
