#pragma once

#include "../atkAudioModule.h"

#include <memory>
#include <string>

namespace atk
{
// One "atkAudio PluginHost" OBS filter instance: hosts a single VST3 plugin and shows its UI in
// a separate window that is owned by the OBS main window.
class PluginHost : public atkAudioModule
{
public:
    PluginHost();
    ~PluginHost();

    // Prepare the processor ahead of the first audio callback, so a restored plugin is
    // instantiated even when the parent source is not producing audio yet.
    void prepare(int numChannels, int numSamples, double sampleRate);

    void process(float** buffer, int numChannels, int numSamples, double sampleRate) override;

    void getState(std::string& s) override;
    void setState(std::string& s) override;
    void setVisible(bool visible) override;

    void setWindowTitle(const std::string& title);
    bool isWindowVisible() const;

    int getInnerPluginChannelCount() const;
    float getCpuLoad() const;
    int getLatencyMs() const;

protected:
    juce::Component* getWindowComponent() override;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace atk
