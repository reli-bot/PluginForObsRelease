#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class PluginHostFooter final
    : public juce::Component
    , private juce::Timer
{
public:
    PluginHostFooter(const juce::String& actionButtonText, bool showNotice = false)
        : showNoticeLabel(showNotice)
    {
        actionButton.setButtonText(actionButtonText);
        addAndMakeVisible(actionButton);

        statsLabel.setFont(juce::FontOptions(10.0f));
        statsLabel.setJustificationType(juce::Justification::centredLeft);
        statsLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
        statsLabel.setBorderSize(juce::BorderSize<int>(0, 4, 0, 0));
        statsLabel.setTooltip("Plugin latency and CPU load");
        addAndMakeVisible(statsLabel);

        if (showNoticeLabel)
        {
            // AGPLv3 "Appropriate Legal Notices" (the full atkAudio build shows these in its About dialog).
            noticeLabel.setText("atkAudio PluginHost (VST3-only) - AGPLv3, no warranty", juce::dontSendNotification);
            noticeLabel.setFont(juce::FontOptions(10.0f));
            noticeLabel.setJustificationType(juce::Justification::centredRight);
            noticeLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
            noticeLabel.setTooltip(
                "Based on atkAudio Plugin for OBS (c) atkAudio, licensed under the GNU AGPLv3 and provided "
                "without any warranty. Source code: the fork this build was made from."
            );
            addAndMakeVisible(noticeLabel);
        }

        startTimerHz(10);
    }

    ~PluginHostFooter() override
    {
        stopTimer();
    }

    void setStatsCallbacks(std::function<float()> getCpuLoadFn, std::function<int()> getLatencyMsFn)
    {
        getCpuLoad = getCpuLoadFn;
        getLatencyMs = getLatencyMsFn;
    }

    void timerCallback() override
    {
        float cpuLoad = getCpuLoad ? getCpuLoad() : 0.0f;
        int latencyMs = getLatencyMs ? getLatencyMs() : 0;

        statsLabel.setText(
            juce::String(latencyMs) + "ms " + juce::String(cpuLoad, 2).replace("0.", "."),
            juce::dontSendNotification
        );
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        const int statsHeight = 14;
        auto statsArea = bounds.removeFromBottom(statsHeight);

        statsLabel.setBounds(statsArea.removeFromLeft(60));

        if (showNoticeLabel)
            noticeLabel.setBounds(statsArea);

        const auto buttonWidth = juce::jmin(bounds.getWidth(), juce::jmax(120, actionButton.getBestWidthForHeight(40)));
        actionButton.setBounds(bounds.withSizeKeepingCentre(buttonWidth, bounds.getHeight()));
    }

    juce::TextButton actionButton;
    juce::Label statsLabel;
    juce::Label noticeLabel;

private:
    bool showNoticeLabel = false;
    std::function<float()> getCpuLoad;
    std::function<int()> getLatencyMs;
    juce::SharedResourcePointer<juce::TooltipWindow> tooltipWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHostFooter)
};
