#include "core/atkaudio/atkaudio.h"

#include <atkaudio/Logging.h>
#include <atkaudio/ObsJucePluginFormatLifecycle.h>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <atomic>

namespace
{
std::atomic<int> g_liveHostInstances{0};
} // namespace

juce::File atk::getSettingsFile(const juce::String& name)
{
    auto* module = obs_current_module();
    if (module == nullptr)
    {
        atk::logging::error("SETTINGS", "Settings path resolution requires an OBS module context");
        return {};
    }

    auto filename = name + ".settings";
    char* obsPath = obs_module_get_config_path(module, filename.toRawUTF8());
    if (obsPath == nullptr)
    {
        const char* binaryPath = obs_get_module_binary_path(module);
        atk::logging::error(
            "SETTINGS",
            "obs_module_get_config_path returned nullptr for settings file: "
                + name
                + ", moduleBinaryPath="
                + juce::String(binaryPath ? binaryPath : "(unknown)")
        );
        return {};
    }

    juce::File result(juce::String::fromUTF8(obsPath));
    bfree(obsPath);

    auto parentDir = result.getParentDirectory();
    if (!parentDir.exists() && !parentDir.createDirectory())
    {
        atk::logging::error(
            "SETTINGS",
            "failed to create settings directory for \"" + name + "\"; path=\"" + parentDir.getFullPathName() + "\""
        );
    }

    return result;
}

bool atk::create()
{
    atk::logging::debug("LIFECYCLE", "atk::create begin");

    auto& lifecycle = atk::ObsJucePluginFormatLifecycle::getInstance();
    if (!lifecycle.initialize())
    {
        atk::logging::error("LIFECYCLE", "atk::create failed to initialize OBS JUCE lifecycle");
        return false;
    }

    atk::logging::debug("LIFECYCLE", "atk::create completed");
    return true;
}

void atk::pump()
{
    atk::ObsJucePluginFormatLifecycle::getInstance().pumpPendingMessages();
}

bool atk::isReady()
{
    return atk::ObsJucePluginFormatLifecycle::getInstance().isReady();
}

bool atk::isShuttingDown()
{
    return atk::ObsJucePluginFormatLifecycle::getInstance().isShuttingDown();
}

void atk::registerHostInstance()
{
    g_liveHostInstances.fetch_add(1, std::memory_order_acq_rel);
}

void atk::unregisterHostInstance()
{
    g_liveHostInstances.fetch_sub(1, std::memory_order_acq_rel);
}

void atk::destroy()
{
    atk::logging::debug("LIFECYCLE", "atk::destroy begin");

    auto& lifecycle = atk::ObsJucePluginFormatLifecycle::getInstance();

    // PluginHost instances destroyed off the message thread delete themselves asynchronously.
    // Give those queued deletions a chance to run (on this, the message thread) so hosted VST3
    // plugins are released before the JUCE runtime goes away.
    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (messageManager->isThisTheMessageThread())
        {
            const auto deadline = juce::Time::getMillisecondCounter() + 3000;
            while (g_liveHostInstances.load(std::memory_order_acquire) > 0
                   && juce::Time::getMillisecondCounter() < deadline)
            {
                messageManager->runDispatchLoopUntil(10);
            }
        }
    }

    if (const auto remaining = g_liveHostInstances.load(std::memory_order_acquire); remaining > 0)
        atk::logging::warning("LIFECYCLE", juce::String(remaining) + " PluginHost instance(s) still alive at shutdown");

    lifecycle.shutdown();

    atk::logging::debug("LIFECYCLE", "atk::destroy completed");
}

void* atk::getObsMainWindowHandle()
{
    return obs_frontend_get_main_window_handle();
}
