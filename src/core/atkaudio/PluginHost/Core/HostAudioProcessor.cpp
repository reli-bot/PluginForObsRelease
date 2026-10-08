#include "HostAudioProcessor.h"
#include "../../SharedPluginList.h"
#include "../UI/HostEditorWindow.h"

#include <atkaudio/Logging.h>

#include <optional>

using namespace juce;
using juce::NullCheckedInvocation;

juce::Optional<juce::AudioPlayHead::PositionInfo> HostAudioProcessorImpl::AtkAudioPlayHead::getPosition() const
{
    return positionInfo;
}

AudioChannelSet HostAudioProcessorImpl::getChannelSetForCount(int numChannels)
{
    if (numChannels <= 0 || numChannels == 2)
        return AudioChannelSet::stereo();
    if (numChannels == 1)
        return AudioChannelSet::mono();

    // For common surround formats, use the named sets
    switch (numChannels)
    {
    case 3:
        return AudioChannelSet::createLCR();
    case 4:
        return AudioChannelSet::quadraphonic();
    case 5:
        return AudioChannelSet::create5point0();
    case 6:
        return AudioChannelSet::create5point1();
    case 7:
        return AudioChannelSet::create7point0();
    case 8:
        return AudioChannelSet::create7point1();
    default:
        return AudioChannelSet::discreteChannels(numChannels);
    }
}

HostAudioProcessorImpl::HostAudioProcessorImpl(int numChannels)
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", getChannelSetForCount(numChannels), true)
              .withOutput("Output", getChannelSetForCount(numChannels), true)
              .withInput("Sidechain", getChannelSetForCount(numChannels), false)
      )
{
    // VST3 only (see JUCE_PLUGINHOST_* definitions in src/CMakeLists.txt).
    juce::addDefaultFormatsToManager(pluginFormatManager);

    // The known-plugin list is read from the cache file only; nothing is scanned here.
    atk::SharedPluginList::getInstance()->loadPluginList(pluginList, true);
    pluginList.addChangeListener(this);

    routingMatrix.initializeDefaultMapping(numChannels * 2);

    atk::logging::debug("HostAudioProcessorImpl::ctor", "initialized PluginHost processor");
}

HostAudioProcessorImpl::~HostAudioProcessorImpl()
{
    pluginList.removeChangeListener(this);
    atk::logging::debug("HostAudioProcessorImpl::dtor", "destroyed PluginHost processor");
}

bool HostAudioProcessorImpl::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& mainOutput = layouts.getMainOutputChannelSet();
    const auto& mainInput = layouts.getMainInputChannelSet();

    if (!mainInput.isDisabled() && mainInput != mainOutput)
        return false;

    if (mainOutput.size() > 8)
        return false;

    return true;
}

void HostAudioProcessorImpl::configureInnerProcessingUnlocked(double sampleRate, int samplesPerBlock)
{
    if (inner == nullptr)
        return;

    bool layoutSupported = false;
    auto layout = getBusesLayout();

    if (inner->checkBusesLayoutSupported(layout))
    {
        inner->setBusesLayout(layout);
        layoutSupported = true;
    }

    if (!layoutSupported && layout.inputBuses.size() > 1)
    {
        layout.inputBuses.removeLast();
        layout.inputBuses.add(AudioChannelSet::stereo());
        if (inner->checkBusesLayoutSupported(layout))
        {
            inner->setBusesLayout(layout);
            layoutSupported = true;
        }
    }

    if (!layoutSupported && layout.inputBuses.size() > 1)
    {
        layout.inputBuses.removeLast();
        layout.inputBuses.add(AudioChannelSet::mono());
        if (inner->checkBusesLayoutSupported(layout))
        {
            inner->setBusesLayout(layout);
            layoutSupported = true;
        }
    }

    if (!layoutSupported && layout.inputBuses.size() > 1)
    {
        layout.inputBuses.removeLast();
        if (inner->checkBusesLayoutSupported(layout))
        {
            inner->setBusesLayout(layout);
            layoutSupported = true;
        }
    }

    if (layoutSupported)
    {
        inner->setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
        return;
    }

    auto pluginInputs = inner->getTotalNumInputChannels();
    auto pluginOutputs = inner->getTotalNumOutputChannels();
    auto hostInputs = getMainBusNumInputChannels();
    auto hostOutputs = getMainBusNumOutputChannels();

    inner->setPlayConfigDetails(
        jmin(pluginInputs, hostInputs),
        jmin(pluginOutputs, hostOutputs),
        sampleRate,
        samplesPerBlock
    );
}

void HostAudioProcessorImpl::prepareToPlay(double sr, int bs)
{
    const ScopedLock sl(innerMutex);

    const bool wasActive = active;

    active = true;

    // Keep an identity OBS<->plugin mapping for every channel (main bus + sidechain).
    routingMatrix.resizeMappings(jmax(1, getTotalNumInputChannels()));

    if (inner != nullptr)
    {
        if (wasActive && juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            inner->releaseResources();

        configureInnerProcessingUnlocked(sr, bs);
        inner->prepareToPlay(sr, bs);
    }

    const int maxChannels = 32;
    const int maxSamples = bs * 2;

    if (internalBuffer.getNumChannels() < maxChannels || internalBuffer.getNumSamples() < maxSamples)
        internalBuffer.setSize(maxChannels, maxSamples, false, false, true);

    if (hasPendingPlugin)
    {
        hasPendingPlugin = false;
        setNewPlugin(pendingPluginDescription, pendingEditorStyle, pendingPluginState);
        pendingPluginState.reset();
    }
}

void HostAudioProcessorImpl::releaseResources()
{
    const ScopedLock sl(innerMutex);

    active = false;

    if (inner != nullptr)
    {
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            inner->releaseResources();
    }
}

void HostAudioProcessorImpl::reset()
{
    const ScopedLock sl(innerMutex);

    if (inner != nullptr)
        inner->reset();
}

void HostAudioProcessorImpl::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midiBuffer)
{
    const ScopedTryLock sl(innerMutex);
    if (!sl.isLocked())
        return;

    jassert(!isUsingDoublePrecision());
    if (inner == nullptr)
        return;

    int numOutputChannels = 0;
    for (int i = 0; i < inner->getBusCount(false); ++i)
        numOutputChannels += inner->getChannelCountOfBus(false, i);

    if (numOutputChannels == 0)
        return;

    atkPlayHead.positionInfo.setIsPlaying(true);
    atkPlayHead.positionInfo.setBpm(120.0);
    auto pos =
        atkPlayHead.positionInfo.getTimeInSamples().hasValue() ? *atkPlayHead.positionInfo.getTimeInSamples() : 0;
    atkPlayHead.positionInfo.setTimeInSamples(pos + buffer.getNumSamples());
    inner->setPlayHead(&atkPlayHead);

    const int numSamples = buffer.getNumSamples();
    const int pluginChannels = buffer.getNumChannels();
    if (internalBuffer.getNumChannels() < pluginChannels || internalBuffer.getNumSamples() < numSamples)
        internalBuffer.setSize(jmax(pluginChannels, internalBuffer.getNumChannels()), numSamples, false, false, true);

    routingMatrix
        .applyInputRouting(buffer.getArrayOfWritePointers(), noDeviceInputs, internalBuffer, pluginChannels, numSamples, 0);

    AudioBuffer<float> tempBuffer;
    tempBuffer.setDataToReferTo(internalBuffer.getArrayOfWritePointers(), internalBuffer.getNumChannels(), numSamples);

    // No MIDI devices in the VST3-only build: the plugin always gets an empty MIDI buffer.
    midiBuffer.clear();

    if (inner->isSuspended())
        tempBuffer.clear();
    else
        inner->processBlock(tempBuffer, midiBuffer);

    routingMatrix.applyOutputRouting(
        internalBuffer,
        buffer.getArrayOfWritePointers(),
        noDeviceOutputs,
        pluginChannels,
        numSamples,
        0
    );
}

void HostAudioProcessorImpl::processBlock(AudioBuffer<double>&, MidiBuffer&)
{
    jassert(isUsingDoublePrecision());
}

bool HostAudioProcessorImpl::hasEditor() const
{
    return false;
}

AudioProcessorEditor* HostAudioProcessorImpl::createEditor()
{
    return nullptr;
}

const String HostAudioProcessorImpl::getName() const
{
    return "atkAudio PluginHost";
}

bool HostAudioProcessorImpl::acceptsMidi() const
{
    return false;
}

bool HostAudioProcessorImpl::producesMidi() const
{
    return false;
}

double HostAudioProcessorImpl::getTailLengthSeconds() const
{
    return 0.0;
}

int HostAudioProcessorImpl::getNumPrograms()
{
    return 0;
}

int HostAudioProcessorImpl::getCurrentProgram()
{
    return 0;
}

void HostAudioProcessorImpl::setCurrentProgram(int)
{
}

const String HostAudioProcessorImpl::getProgramName(int)
{
    return "None";
}

void HostAudioProcessorImpl::changeProgramName(int, const String&)
{
}

void HostAudioProcessorImpl::setInputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    routingMatrix.setInputMapping(mapping);
}

std::vector<std::vector<bool>> HostAudioProcessorImpl::getInputChannelMapping() const
{
    return routingMatrix.getInputMapping();
}

void HostAudioProcessorImpl::setOutputChannelMapping(const std::vector<std::vector<bool>>& mapping)
{
    routingMatrix.setOutputMapping(mapping);
}

std::vector<std::vector<bool>> HostAudioProcessorImpl::getOutputChannelMapping() const
{
    return routingMatrix.getOutputMapping();
}

void HostAudioProcessorImpl::getStateInformation(MemoryBlock& destData)
{
    XmlElement xml("state");

    {
        auto inputMapping = routingMatrix.getInputMapping();
        auto outputMapping = routingMatrix.getOutputMapping();

        if (!inputMapping.empty())
        {
            auto inputMappingElement = std::make_unique<XmlElement>("InputMapping");
            for (const auto& row : inputMapping)
            {
                String rowData;
                for (bool cell : row)
                    rowData += cell ? '1' : '0';

                auto rowElement = std::make_unique<XmlElement>("Row");
                rowElement->setAttribute("data", rowData);
                inputMappingElement->addChildElement(rowElement.release());
            }
            xml.addChildElement(inputMappingElement.release());
        }

        if (!outputMapping.empty())
        {
            auto outputMappingElement = std::make_unique<XmlElement>("OutputMapping");
            for (const auto& row : outputMapping)
            {
                String rowData;
                for (bool cell : row)
                    rowData += cell ? '1' : '0';

                auto rowElement = std::make_unique<XmlElement>("Row");
                rowElement->setAttribute("data", rowData);
                outputMappingElement->addChildElement(rowElement.release());
            }
            xml.addChildElement(outputMappingElement.release());
        }
    }

    {
        // The hosted plugin is only created, replaced or destroyed on the message thread. There the
        // state can be read without taking innerMutex, so the audio thread keeps processing (VST3
        // allows getState on the UI thread during processing). Elsewhere, lock as usual.
        std::optional<ScopedLock> sl;
        if (!MessageManager::existsAndIsCurrentThread())
            sl.emplace(innerMutex);

        if (inner != nullptr)
        {
            xml.setAttribute(editorStyleTag, (int)editorStyle);

            auto pd = inner->getPluginDescription();

            if (pd.pluginFormatName == "VST3" && pd.fileOrIdentifier.contains("/Contents/"))
                pd.fileOrIdentifier = pd.fileOrIdentifier.upToLastOccurrenceOf(".vst3", true, false);

            xml.addChildElement(pd.createXml().release());

            MemoryBlock innerState;
            inner->getStateInformation(innerState);

            auto stateNode = std::make_unique<XmlElement>(innerStateTag);
            stateNode->addTextElement(innerState.toBase64Encoding());
            xml.addChildElement(stateNode.release());
        }
        else if (hasPendingPlugin)
        {
            // The plugin has not been instantiated yet (no audio has flowed since the state was
            // restored). Keep the pending plugin and its state so saving does not drop them.
            xml.setAttribute(editorStyleTag, (int)pendingEditorStyle);
            xml.addChildElement(pendingPluginDescription.createXml().release());

            auto stateNode = std::make_unique<XmlElement>(innerStateTag);
            stateNode->addTextElement(pendingPluginState.toBase64Encoding());
            xml.addChildElement(stateNode.release());
        }
    }

    const auto text = xml.toString();
    destData.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void HostAudioProcessorImpl::setStateInformation(const void* data, int sizeInBytes)
{
    const ScopedLock sl(innerMutex);

    if (data == nullptr || sizeInBytes <= 0)
        return;

    auto xml = XmlDocument::parse(String(CharPointer_UTF8(static_cast<const char*>(data)), (size_t)sizeInBytes));
    if (xml == nullptr)
    {
        atk::logging::warning("HostAudioProcessorImpl::setStateInformation", "ignoring unreadable saved state");
        return;
    }

    // Saved states may also contain "audioClientState"/"midiClientState" (audio/MIDI device
    // routing from the full atkAudio build). Those features do not exist here and are ignored.

    {
        if (auto* inputMappingElement = xml->getChildByName("InputMapping"))
        {
            std::vector<std::vector<bool>> inputMapping;
            for (auto* rowElement : inputMappingElement->getChildIterator())
            {
                String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                inputMapping.push_back(row);
            }
            if (!inputMapping.empty())
                routingMatrix.setInputMapping(inputMapping);
        }
        else if (xml->hasAttribute("inputChannelMapping"))
        {
            auto inputMapping = routingMatrix.getInputMapping();
            auto inputMappingStr = xml->getStringAttribute("inputChannelMapping");
            auto tokens = StringArray::fromTokens(inputMappingStr, ";", "");

            for (auto& row : inputMapping)
                std::fill(row.begin(), row.end(), false);

            for (const auto& token : tokens)
            {
                auto parts = StringArray::fromTokens(token, ",", "");
                if (parts.size() == 2)
                {
                    int obsChannel = parts[0].getIntValue();
                    int pluginChannel = parts[1].getIntValue();
                    if (obsChannel >= 0
                        && obsChannel < (int)inputMapping.size()
                        && pluginChannel >= 0
                        && pluginChannel < (int)inputMapping[obsChannel].size())
                    {
                        inputMapping[obsChannel][pluginChannel] = true;
                    }
                }
            }

            routingMatrix.setInputMapping(inputMapping);
        }

        if (auto* outputMappingElement = xml->getChildByName("OutputMapping"))
        {
            std::vector<std::vector<bool>> outputMapping;
            for (auto* rowElement : outputMappingElement->getChildIterator())
            {
                String rowData = rowElement->getStringAttribute("data");
                std::vector<bool> row;
                for (int i = 0; i < rowData.length(); ++i)
                    row.push_back(rowData[i] == '1');
                outputMapping.push_back(row);
            }
            if (!outputMapping.empty())
                routingMatrix.setOutputMapping(outputMapping);
        }
        else if (xml->hasAttribute("outputChannelMapping"))
        {
            auto outputMapping = routingMatrix.getOutputMapping();
            auto outputMappingStr = xml->getStringAttribute("outputChannelMapping");
            auto tokens = StringArray::fromTokens(outputMappingStr, ";", "");

            for (auto& row : outputMapping)
                std::fill(row.begin(), row.end(), false);

            for (const auto& token : tokens)
            {
                auto parts = StringArray::fromTokens(token, ",", "");
                if (parts.size() == 2)
                {
                    int pluginChannel = parts[0].getIntValue();
                    int obsChannel = parts[1].getIntValue();
                    if (pluginChannel >= 0
                        && pluginChannel < (int)outputMapping.size()
                        && obsChannel >= 0
                        && obsChannel < (int)outputMapping[pluginChannel].size())
                    {
                        outputMapping[pluginChannel][obsChannel] = true;
                    }
                }
            }

            routingMatrix.setOutputMapping(outputMapping);
        }
    }

    if (auto* pluginNode = xml->getChildByName("PLUGIN"))
    {
        PluginDescription pd;
        pd.loadFromXml(*pluginNode);

        if (pd.pluginFormatName == "VST3" && pd.fileOrIdentifier.contains("/Contents/"))
            pd.fileOrIdentifier = pd.fileOrIdentifier.upToLastOccurrenceOf(".vst3", true, false);

        MemoryBlock innerState;
        innerState.fromBase64Encoding(xml->getChildElementAllSubText(innerStateTag, {}));

        setNewPlugin(pd, (EditorStyle)xml->getIntAttribute(editorStyleTag, 0), innerState);
    }
}

void HostAudioProcessorImpl::setNewPlugin(const PluginDescription& pd, EditorStyle where, const MemoryBlock& mb)
{
    const ScopedLock sl(innerMutex);

    if (!active)
    {
        pendingPluginDescription = pd;
        pendingPluginState = mb;
        pendingEditorStyle = where;
        hasPendingPlugin = true;
        return;
    }

    const auto callback = [this, where, mb](std::unique_ptr<AudioPluginInstance> instance, const String& error)
    {
        const ScopedLock sl(innerMutex);
        if (error.isNotEmpty() || instance == nullptr)
        {
            const auto message = error.isNotEmpty() ? error : String("Unknown error");
            atk::logging::warning("PluginHost", "plugin load failed: " + message);

            auto options =
                MessageBoxOptions::makeOptionsOk(MessageBoxIconType::WarningIcon, "Plugin Load Failed", message);
            messageBox = AlertWindow::showScopedAsync(options, nullptr);
            return;
        }

        bool needsPluginChanged = false;
        String innerName;
        if (inner != nullptr)
            innerName = inner->getPluginDescription().descriptiveName;

        if (innerName != instance->getPluginDescription().descriptiveName)
            needsPluginChanged = true;

        if (needsPluginChanged)
        {
            atk::logging::info("PluginHost", "loaded " + instance->getPluginDescription().descriptiveName);
            inner = std::move(instance);
        }

        editorStyle = where;

        if (active && inner != nullptr)
        {
            const auto sampleRate = getSampleRate();
            const auto blockSize = getBlockSize();

            if (sampleRate > 0.0 && blockSize > 0)
            {
                configureInnerProcessingUnlocked(sampleRate, blockSize);
                inner->prepareToPlay(sampleRate, blockSize);
            }
        }

        if (inner != nullptr && !mb.isEmpty())
            inner->setStateInformation(mb.getData(), (int)mb.getSize());

        if (needsPluginChanged)
            NullCheckedInvocation::invoke(pluginChanged);
    };

    if (inner == nullptr || (inner != nullptr && inner->getPluginDescription().name != pd.name))
        pluginFormatManager.createPluginInstanceAsync(pd, getSampleRate(), getBlockSize(), callback);
}

void HostAudioProcessorImpl::clearPlugin()
{
    std::unique_ptr<AudioPluginInstance> pluginToDestroy;
    {
        const ScopedLock sl(innerMutex);
        hasPendingPlugin = false;
        pendingPluginState.reset();
        pluginToDestroy = std::move(inner);
    }

    if (pluginToDestroy != nullptr)
    {
        pluginToDestroy->suspendProcessing(true);
        pluginToDestroy = nullptr;
    }

    NullCheckedInvocation::invoke(pluginChanged);
}

bool HostAudioProcessorImpl::isPluginLoaded() const
{
    const ScopedLock sl(innerMutex);
    return inner != nullptr;
}

juce::String HostAudioProcessorImpl::getLoadedPluginName() const
{
    const ScopedLock sl(innerMutex);
    return inner != nullptr ? inner->getName() : juce::String();
}

void HostAudioProcessorImpl::reloadPluginList()
{
    pluginList.removeChangeListener(this);
    pluginList.clear();
    atk::SharedPluginList::getInstance()->loadPluginList(pluginList, true);
    pluginList.addChangeListener(this);
}

std::unique_ptr<AudioProcessorEditor> HostAudioProcessorImpl::createInnerEditor() const
{
    // Avoid holding innerMutex during editor creation: the audio thread's tryLock would fail and
    // drop to pass-through while a (possibly slow) plugin editor is being created.
    AudioPluginInstance* pluginToUse = nullptr;
    {
        const ScopedLock sl(innerMutex);
        pluginToUse = inner.get();
    }

    if (pluginToUse == nullptr)
        return nullptr;

    if (pluginToUse->hasEditor())
        if (auto* pluginEditor = pluginToUse->createEditorAndMakeActive())
            return rawToUniquePtr(pluginEditor);

    // Plugins without their own UI get JUCE's generic parameter editor.
    return std::make_unique<GenericAudioProcessorEditor>(*pluginToUse);
}

EditorStyle HostAudioProcessorImpl::getEditorStyle() const noexcept
{
    return editorStyle;
}

juce::AudioPluginInstance* HostAudioProcessorImpl::getInnerPlugin() const
{
    const ScopedLock sl(innerMutex);
    return inner.get();
}

void HostAudioProcessorImpl::changeListenerCallback(ChangeBroadcaster* source)
{
    if (source == &pluginList)
        atk::SharedPluginList::getInstance()->savePluginList(pluginList);
}

bool HostAudioProcessor::hasEditor() const
{
    return true;
}

AudioProcessorEditor* HostAudioProcessor::createEditor()
{
    return new HostAudioProcessorEditor(*this);
}
