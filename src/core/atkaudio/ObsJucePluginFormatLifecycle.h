#pragma once

#include <atomic>
#include <memory>

namespace juce
{
class ScopedJuceInitialiser_GUI;
}

namespace atk
{

enum class ObsJucePluginFormatLifecyclePhase
{
    Uninitialized,
    Initializing,
    Ready,
    ShuttingDown,
    Shutdown,
};

class ObsJucePluginFormatLifecycle
{
public:
    static ObsJucePluginFormatLifecycle& getInstance();

    bool initialize();
    void pumpPendingMessages();
    void shutdown();

    ObsJucePluginFormatLifecyclePhase phase() const;
    bool isReady() const;
    bool isShuttingDown() const;

private:
    ObsJucePluginFormatLifecycle();
    ~ObsJucePluginFormatLifecycle();

    std::atomic<ObsJucePluginFormatLifecyclePhase> lifecyclePhase{ObsJucePluginFormatLifecyclePhase::Uninitialized};

    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceRuntime;
};

} // namespace atk
