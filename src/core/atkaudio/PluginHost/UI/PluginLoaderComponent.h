#pragma once

#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/Logging.h>
#include <atkaudio/SandboxedPluginScanner.h>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>

// JUCE utility functions
using juce::isPositiveAndBelow;
using juce::NullCheckedInvocation;

// Shows the cached list of known VST3 plugins. Scanning only happens when the user asks for it
// ("Options..." > "Scan for new or updated VST3 plug-ins"); each plugin file is scanned in the
// separate atkaudio-pluginforobs_scanner process so a misbehaving plugin cannot take OBS down.
class PluginLoaderComponent final : public juce::Component
{
public:
    template <typename Callback>
    PluginLoaderComponent(
        juce::AudioPluginFormatManager& manager,
        juce::KnownPluginList& list,
        juce::PropertiesFile* props,
        HostAudioProcessorImpl* processorPtr,
        Callback&& callback
    )
        : pluginListComponent(
              manager,
              list,
              props != nullptr ? props->getFile().getSiblingFile("RecentlyCrashedPluginsList") : juce::File{},
              props,
              false
          )
        , processor(processorPtr)
        , footer("Load plugin", true)
    {
        pluginListComponent.getTableListBox().setMultipleSelectionEnabled(false);

        auto sandboxedScanner = std::make_unique<atk::SandboxedScanner>();
        sandboxedScanner->setFormatManager(&manager);
        sandboxedScanner->setKnownPluginList(&list);
        if (sandboxedScanner->isScannerAvailable())
            atk::logging::debug("PluginLoaderComponent::ctor", "using sandboxed plugin scanner");
        else
            atk::logging::warning(
                "PluginLoaderComponent::ctor",
                "sandboxed scanner unavailable; plugin scanning will fall back to in-process"
            );
        list.setCustomScanner(std::move(sandboxedScanner));

        addAndMakeVisible(pluginListComponent);
        addAndMakeVisible(footer);

        const auto getCallback = [this, &list, cb = std::forward<Callback>(callback)](EditorStyle style)
        {
            return [this, &list, cb, style]
            {
                const auto index = pluginListComponent.getTableListBox().getSelectedRow();
                const auto& types = list.getTypes();

                if (isPositiveAndBelow(index, types.size()))
                    NullCheckedInvocation::invoke(cb, types.getReference(index), style);
            };
        };

        footer.actionButton.onClick = getCallback(EditorStyle::thisWindow);

        // Set up CPU/latency stats callbacks (use lambdas to read from processor dynamically)
        if (processor)
        {
            footer.setStatsCallbacks(
                [this]() -> float
                {
                    if (processor && processor->getCpuLoad)
                        return processor->getCpuLoad();
                    return 0.0f;
                },
                [this]() -> int
                {
                    if (processor && processor->getLatencyMs)
                        return processor->getLatencyMs();
                    return 0;
                }
            );
        }
    }

    void resized() override
    {
        doLayout(&pluginListComponent, footer, 54, getLocalBounds());
    }

private:
    juce::PluginListComponent pluginListComponent;
    HostAudioProcessorImpl* processor = nullptr;
    PluginHostFooter footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginLoaderComponent)
};
