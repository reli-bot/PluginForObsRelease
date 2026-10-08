// Test plugin for the CI run of OBS 31.0.4 with the PluginHost filter (not shipped).
// It applies its "Gain" parameter and appends one line per second of audio to
// %ATK_PROBE_LOG_DIR%\probe<ATK_PROBE_ID>.log with the input/output RMS it measured.
// With ATK_PROBE_SELF_SET=1 it changes its own gain to 0.25 after three seconds of audio,
// like a user turning a knob, so the CI can check that OBS saves and restores the new value.

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

class ProbeProcessor final : public juce::AudioProcessor
{
public:
    ProbeProcessor()
        : AudioProcessor(
              BusesProperties()
                  .withInput("Input", juce::AudioChannelSet::stereo(), true)
                  .withOutput("Output", juce::AudioChannelSet::stereo(), true)
          )
    {
        addParameter(
            gain = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, ATK_PROBE_DEFAULT_GAIN)
        );

        selfSet = juce::SystemStats::getEnvironmentVariable("ATK_PROBE_SELF_SET", {}) == "1";

        const auto dir = juce::SystemStats::getEnvironmentVariable("ATK_PROBE_LOG_DIR", {});
        if (dir.isNotEmpty())
            logFile = juce::File(dir).getChildFile("probe" + juce::String(ATK_PROBE_ID) + ".log");

        log("created");
    }

    ~ProbeProcessor() override
    {
        log("destroyed");
    }

    const juce::String getName() const override
    {
        return JucePlugin_Name;
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const juce::String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto& in = layouts.getMainInputChannelSet();
        const auto& out = layouts.getMainOutputChannelSet();
        return !out.isDisabled()
            && in == out
            && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
    }

    void prepareToPlay(double sr, int bs) override
    {
        sampleRate = sr;
        log("prepare sr=" + juce::String(sr, 0) + " bs=" + juce::String(bs));
    }

    void releaseResources() override
    {
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int n = buffer.getNumSamples();
        const int ch = juce::jmin(buffer.getNumChannels(), getTotalNumInputChannels());

        double inSum = 0.0;
        double outSum = 0.0;

        for (int c = 0; c < ch; ++c)
        {
            const auto* d = buffer.getReadPointer(c);
            for (int i = 0; i < n; ++i)
                inSum += (double)d[i] * d[i];
        }

        const float g = gain->get();
        for (int c = 0; c < ch; ++c)
            buffer.applyGain(c, 0, n, g);

        for (int c = 0; c < ch; ++c)
        {
            const auto* d = buffer.getReadPointer(c);
            for (int i = 0; i < n; ++i)
                outSum += (double)d[i] * d[i];
        }

        accIn += inSum;
        accOut += outSum;
        accValues += (long long)n * juce::jmax(1, ch);
        totalSamples += n;
        ++blocks;

        const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;

        if (selfSet && !didSelfSet && totalSamples > (long long)(3.0 * rate))
        {
            didSelfSet = true;
            gain->setValueNotifyingHost(0.25f);
            log("self-set gain to 0.25");
        }

        if (totalSamples - lastLogAt >= (long long)rate)
        {
            lastLogAt = totalSamples;
            const auto values = (double)juce::jmax(1LL, accValues);
            log(
                juce::String::formatted(
                    "process blocks=%lld ch=%d sr=%.0f gain=%.3f in_rms=%.4f out_rms=%.4f",
                    blocks,
                    ch,
                    rate,
                    (double)g,
                    std::sqrt(accIn / values),
                    std::sqrt(accOut / values)
                )
            );
            accIn = 0.0;
            accOut = 0.0;
            accValues = 0;
        }
    }

    bool hasEditor() const override
    {
        return true;
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        log("editor created");
        return new juce::GenericAudioProcessorEditor(*this);
    }

    void getStateInformation(juce::MemoryBlock& dest) override
    {
        juce::XmlElement xml("PROBE");
        xml.setAttribute("gain", (double)gain->get());
        copyXmlToBinary(xml, dest);
        log("getState gain=" + juce::String(gain->get(), 3));
    }

    void setStateInformation(const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary(data, size))
            if (xml->hasTagName("PROBE"))
                *gain = (float)xml->getDoubleAttribute("gain", ATK_PROBE_DEFAULT_GAIN);

        log("setState gain=" + juce::String(gain->get(), 3));
    }

private:
    void log(const juce::String& message)
    {
        if (logFile == juce::File())
            return;

        const juce::ScopedLock sl(logLock);
        logFile.appendText(
            juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n",
            false,
            false,
            "\n"
        );
    }

    juce::AudioParameterFloat* gain = nullptr;
    bool selfSet = false;
    bool didSelfSet = false;
    double sampleRate = 0.0;

    double accIn = 0.0;
    double accOut = 0.0;
    long long accValues = 0;
    long long totalSamples = 0;
    long long lastLogAt = 0;
    long long blocks = 0;

    juce::File logFile;
    juce::CriticalSection logLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProbeProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ProbeProcessor();
}
