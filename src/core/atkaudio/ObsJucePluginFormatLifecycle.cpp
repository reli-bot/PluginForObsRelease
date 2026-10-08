#include "ObsJucePluginFormatLifecycle.h"

#include <atkaudio/Logging.h>

#include <juce_events/juce_events.h>

namespace atk
{

ObsJucePluginFormatLifecycle::ObsJucePluginFormatLifecycle() = default;
ObsJucePluginFormatLifecycle::~ObsJucePluginFormatLifecycle() = default;

ObsJucePluginFormatLifecycle& ObsJucePluginFormatLifecycle::getInstance()
{
    static ObsJucePluginFormatLifecycle instance;
    return instance;
}

bool ObsJucePluginFormatLifecycle::initialize()
{
    const auto currentPhase = lifecyclePhase.load(std::memory_order_acquire);

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Ready)
    {
        atk::logging::debug("Lifecycle.Internal", "initialize skipped: already ready");
        return true;
    }

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Initializing
        || currentPhase == ObsJucePluginFormatLifecyclePhase::ShuttingDown)
    {
        atk::logging::warning("Lifecycle.Internal", "initialize rejected: lifecycle busy");
        return false;
    }

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Initializing, std::memory_order_release);
    atk::logging::debug("Lifecycle.Internal", "initialize begin");

    // Make JUCE treat this host integration as app-context for Windows platform
    // init hooks (notably per-monitor DPI awareness setup in windowing code).
#if JUCE_WINDOWS
    juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return nullptr; };
#endif

    juceRuntime = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager == nullptr)
    {
        juceRuntime.reset();
        lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Uninitialized, std::memory_order_release);
        atk::logging::error("Lifecycle.Internal", "initialize failed: MessageManager unavailable");
        return false;
    }

    // obs_module_load() runs on the OBS UI thread; that thread owns all JUCE windows.
    messageManager->setCurrentThreadAsMessageThread();

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Ready, std::memory_order_release);
    atk::logging::debug("Lifecycle.Internal", "initialize completed");
    return true;
}

void ObsJucePluginFormatLifecycle::pumpPendingMessages()
{
    if (!isReady())
        return;

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (messageManager->isThisTheMessageThread())
            messageManager->runDispatchLoopUntil(0);
    }
}

void ObsJucePluginFormatLifecycle::shutdown()
{
    const auto currentPhase = lifecyclePhase.load(std::memory_order_acquire);

    if (currentPhase == ObsJucePluginFormatLifecyclePhase::Uninitialized
        || currentPhase == ObsJucePluginFormatLifecyclePhase::Shutdown)
        return;

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::ShuttingDown, std::memory_order_release);
    atk::logging::debug("Lifecycle.Internal", "shutdown begin");

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (messageManager->isThisTheMessageThread())
            messageManager->runDispatchLoopUntil(0);
    }

    juceRuntime.reset();

    lifecyclePhase.store(ObsJucePluginFormatLifecyclePhase::Shutdown, std::memory_order_release);
    atk::logging::debug("Lifecycle.Internal", "shutdown completed");
}

ObsJucePluginFormatLifecyclePhase ObsJucePluginFormatLifecycle::phase() const
{
    return lifecyclePhase.load(std::memory_order_acquire);
}

bool ObsJucePluginFormatLifecycle::isReady() const
{
    return phase() == ObsJucePluginFormatLifecyclePhase::Ready;
}

bool ObsJucePluginFormatLifecycle::isShuttingDown() const
{
    return phase() == ObsJucePluginFormatLifecyclePhase::ShuttingDown;
}

} // namespace atk
