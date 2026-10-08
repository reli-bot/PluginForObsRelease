#include "PluginHost.h"
#include "Core/HostAudioProcessor.h"
#include "UI/HostEditorWindow.h"
#include "UI/NativeWindowOwner.h"

#include <atkaudio/Logging.h>
#include <atkaudio/LookAndFeel.h>
#include <atkaudio/atkaudio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>

namespace
{
juce::Colour getWindowBackground()
{
    return juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
}

// Top-level window for one PluginHost: shows the plugin list until a plugin is loaded, then the
// plugin's own editor. It follows the size of its content (plugins resize themselves through their
// own UI), and on Windows it is owned by the OBS main window so it stays above OBS without
// getting its own taskbar button.
class PluginHostWindow final : public juce::DocumentWindow
{
public:
    PluginHostWindow(
        const juce::String& title,
        juce::Component* contentToOwn,
        void* ownerWindowHandleIn,
        std::function<void()> onCloseIn,
        std::function<void(juce::Rectangle<int>)> onBoundsChangedIn
    )
        : juce::DocumentWindow(
              title,
              getWindowBackground(),
              juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton,
              false
          )
        , ownerWindowHandle(ownerWindowHandleIn)
        , onClose(std::move(onCloseIn))
        , onBoundsChanged(std::move(onBoundsChangedIn))
    {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        setContentOwned(contentToOwn, true);
    }

    ~PluginHostWindow() override
    {
        onBoundsChanged = nullptr;
        onClose = nullptr;
        clearContentComponent();
    }

    void showOnDesktop()
    {
        if (!isOnDesktop())
        {
            addToDesktop(getDesktopWindowStyleFlags());
            applyOwner();
        }

        setVisible(true);

        if (isMinimised())
            setMinimised(false);

        toFront(true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

    int getDesktopWindowStyleFlags() const override
    {
        auto flags = juce::DocumentWindow::getDesktopWindowStyleFlags();

        // An owned window does not need its own taskbar button; without an owner keep one so the
        // window cannot get lost behind other applications.
        if (ownerWindowHandle != nullptr)
            flags &= ~juce::ComponentPeer::windowAppearsOnTaskbar;

        return flags;
    }

    void moved() override
    {
        juce::DocumentWindow::moved();
        notifyBounds();
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        notifyBounds();
    }

private:
    void applyOwner()
    {
        if (ownerWindowHandle == nullptr)
            return;

        if (auto* peer = getPeer())
            atk::setNativeWindowOwner(peer->getNativeHandle(), ownerWindowHandle);
    }

    void notifyBounds()
    {
        if (onBoundsChanged && isOnDesktop())
            onBoundsChanged(getBounds());
    }

    void* ownerWindowHandle = nullptr;
    std::function<void()> onClose;
    std::function<void(juce::Rectangle<int>)> onBoundsChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginHostWindow)
};

bool readHeaderLine(std::string& state, const char* key, std::string& valueOut)
{
    const std::string prefix(key);
    if (state.rfind(prefix, 0) != 0)
        return false;

    const auto newlinePos = state.find('\n');
    if (newlinePos == std::string::npos)
        return false;

    valueOut = state.substr(prefix.size(), newlinePos - prefix.size());
    state = state.substr(newlinePos + 1);
    return true;
}
} // namespace

struct atk::PluginHost::Impl : public juce::AsyncUpdater
{
    enum UpdateFlags
    {
        None = 0,
        PrepareProcessor = 1 << 0,
        RestoreState = 1 << 1,
        ShowWindow = 1 << 2,
        DestroyWindow = 1 << 3,
        UpdateTitle = 1 << 4,
    };

    Impl()
        : hostProcessor(std::make_unique<HostAudioProcessor>())
    {
        atk::registerHostInstance();

        hostProcessor->getCpuLoad = [this]() { return getCpuLoad(); };
        hostProcessor->getLatencyMs = [this]() { return getLatencyMs(); };
    }

    ~Impl() override
    {
        shuttingDown.store(true, std::memory_order_release);
        cancelPendingUpdate();

        destroyWindow();

        {
            juce::ScopedLock lock(processorLock);

            if (auto* hostProc = hostProcessor.get())
            {
                hostProc->getCpuLoad = nullptr;
                hostProc->getLatencyMs = nullptr;
                hostProc->suspendProcessing(true);
                hostProc->clearPlugin();
            }

            hostProcessor.reset();
        }

        atk::unregisterHostInstance();
    }

    //==============================================================================
    // Message-thread work

    void handleAsyncUpdate() override
    {
        if (shuttingDown.load(std::memory_order_acquire))
            return;

        auto flags = pendingUpdateFlags.exchange(UpdateFlags::None);

        if (flags & UpdateFlags::PrepareProcessor)
            prepareProcessorOnMessageThread(pendingChannels.load(), pendingSamples.load(), pendingSampleRate.load());

        if (flags & UpdateFlags::RestoreState)
        {
            std::string stateString;
            {
                juce::ScopedLock lock(pendingStateLock);
                stateString = std::move(pendingStateString);
            }
            restoreStateOnMessageThread(stateString);
        }

        if (flags & UpdateFlags::DestroyWindow)
            destroyWindow();

        if (flags & UpdateFlags::ShowWindow)
            showWindow();

        if (flags & UpdateFlags::UpdateTitle)
            if (window != nullptr)
                window->setName(getWindowTitle());
    }

    void post(int flags)
    {
        pendingUpdateFlags.fetch_or(flags);
        triggerAsyncUpdate();
    }

    void prepareProcessorOnMessageThread(int channels, int samples, double rate)
    {
        if (channels <= 0 || samples <= 0 || rate <= 0.0)
            return;

        juce::ScopedLock lock(processorLock);
        auto* processor = hostProcessor.get();
        if (processor == nullptr)
            return;

        juce::ScopedLock callbackLock(processor->getCallbackLock());

        if (isPrepared)
            processor->releaseResources();

        auto layout = juce::AudioProcessor::BusesLayout();
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.outputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));
        layout.inputBuses.add(juce::AudioChannelSet::canonicalChannelSet(channels));

        if (processor->checkBusesLayoutSupported(layout))
        {
            processor->setBusesLayout(layout);
            processor->setRateAndBufferSizeDetails(rate, samples);
            processor->prepareToPlay(rate, samples);

            loadMeasurer.reset(rate, samples);

            isPrepared = true;
        }
    }

    void restoreStateOnMessageThread(const std::string& stateString)
    {
        if (stateString.empty())
            return;

        juce::ScopedLock lock(processorLock);
        auto* processor = hostProcessor.get();
        if (processor == nullptr)
            return;

        juce::ScopedLock callbackLock(processor->getCallbackLock());
        processor->setStateInformation(stateString.data(), (int)stateString.size());
    }

    juce::String getWindowTitle() const
    {
        std::lock_guard<std::mutex> lock(metaMutex);
        return windowTitle.isNotEmpty() ? windowTitle : juce::String("atkAudio PluginHost");
    }

    void showWindow()
    {
        if (shuttingDown.load(std::memory_order_acquire) || hostProcessor == nullptr)
            return;

        if (window == nullptr)
        {
            auto* content = new HostEditorComponent(hostProcessor.get());

            window = std::make_unique<PluginHostWindow>(
                getWindowTitle(),
                content,
                atk::getObsMainWindowHandle(),
                [this]() { onWindowCloseRequested(); },
                [this](juce::Rectangle<int> bounds) { rememberWindowBounds(bounds); }
            );

            placeWindow();
        }

        window->showOnDesktop();
        setWindowVisibleFlag(true);
    }

    void placeWindow()
    {
        if (window == nullptr)
            return;

        juce::Point<int> topLeft;
        bool haveSavedPosition = false;
        {
            std::lock_guard<std::mutex> lock(metaMutex);
            if (hasSavedWindowPosition)
            {
                topLeft = savedWindowPosition;
                haveSavedPosition = true;
            }
        }

        if (haveSavedPosition)
        {
            // Only reuse the saved position if the title bar would be on a connected display.
            const auto titleBarArea = juce::Rectangle<int>(topLeft.x, topLeft.y, juce::jmax(120, window->getWidth()), 30);
            for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
            {
                if (display.userBounds.toNearestInt().intersects(titleBarArea))
                {
                    window->setTopLeftPosition(topLeft);
                    return;
                }
            }
        }

        window->centreWithSize(window->getWidth(), window->getHeight());
    }

    void rememberWindowBounds(juce::Rectangle<int> bounds)
    {
        std::lock_guard<std::mutex> lock(metaMutex);
        lastWindowBounds = bounds;
        hasLastWindowBounds = true;
        savedWindowPosition = bounds.getTopLeft();
        hasSavedWindowPosition = true;
    }

    void onWindowCloseRequested()
    {
        // Called from inside the window's own close handling: hide now, delete it asynchronously.
        setWindowVisibleFlag(false);

        if (window != nullptr)
            window->setVisible(false);

        post(UpdateFlags::DestroyWindow);
    }

    void destroyWindow()
    {
        if (window == nullptr)
            return;

        if (window->isOnDesktop())
            rememberWindowBounds(window->getBounds());

        // Deleting the window deletes the plugin editor (and calls editorBeingDeleted) via
        // HostEditorComponent; the plugin instance itself stays loaded.
        window = nullptr;
    }

    void setVisible(bool visible)
    {
        setWindowVisibleFlag(visible);

        if (visible)
            showWindow();
        else
            destroyWindow();
    }

    void setWindowVisibleFlag(bool visible)
    {
        windowVisible.store(visible, std::memory_order_release);
    }

    bool isWindowVisible() const
    {
        return windowVisible.load(std::memory_order_acquire);
    }

    void setWindowTitle(const juce::String& title)
    {
        if (title.isEmpty())
            return;

        {
            std::lock_guard<std::mutex> lock(metaMutex);
            windowTitle = title;
        }

        post(UpdateFlags::UpdateTitle);
    }

    //==============================================================================
    // Audio thread

    void requestPrepare(int newNumChannels, int newNumSamples, double newSampleRate)
    {
        numChannels = newNumChannels;
        numSamples = juce::jmax(numSamples, newNumSamples);
        sampleRate = newSampleRate;

        syncBuffer.setSize(newNumChannels * 2, numSamples, false, false, true);
        syncMidiBuffer.ensureSize(256);

        pendingChannels.store(newNumChannels);
        pendingSamples.store(numSamples);
        pendingSampleRate.store(newSampleRate);
        post(UpdateFlags::PrepareProcessor);
    }

    void process(float** buffer, int newNumChannels, int newNumSamples, double newSampleRate)
    {
        if (!buffer)
            return;

        const bool needsReconfiguration =
            numChannels != newNumChannels || numSamples < newNumSamples || sampleRate != newSampleRate;

        if (needsReconfiguration)
        {
            // Pass audio through unchanged until the processor is prepared on the message thread.
            requestPrepare(newNumChannels, newNumSamples, newSampleRate);
            return;
        }

        if (!isPrepared)
            return;

        if (!processorLock.tryEnter())
        {
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        auto* processor = hostProcessor.get();
        if (processor == nullptr)
        {
            processorLock.exit();
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        if (!processor->getCallbackLock().tryEnter())
        {
            processorLock.exit();
            for (int ch = 0; ch < newNumChannels * 2; ++ch)
                juce::FloatVectorOperations::clear(buffer[ch], newNumSamples);
            return;
        }

        syncBuffer.setDataToReferTo(buffer, newNumChannels * 2, newNumSamples);
        syncMidiBuffer.clear();

        if (!processor->isSuspended())
        {
            juce::AudioProcessLoadMeasurer::ScopedTimer timer(loadMeasurer, newNumSamples);
            processor->processBlock(syncBuffer, syncMidiBuffer);
        }

        processor->getCallbackLock().exit();
        processorLock.exit();
    }

    //==============================================================================
    // State

    void getState(std::string& s)
    {
        s.clear();

        juce::MemoryBlock state;

        {
            auto* processor = hostProcessor.get();
            if (processor == nullptr)
                return;

            const bool onMessageThread = juce::MessageManager::existsAndIsCurrentThread();

            if (onMessageThread)
            {
                // The hosted plugin is only replaced or destroyed on the message thread, so the
                // state can be read without blocking the audio thread (no dropout while OBS saves).
                processor->getStateInformation(state);
            }
            else
            {
                juce::ScopedLock lock(processorLock);
                juce::ScopedLock callbackLock(processor->getCallbackLock());
                processor->getStateInformation(state);
            }
        }

        auto stateString = state.toString().toStdString();

        juce::Rectangle<int> bounds;
        bool haveBounds = false;
        {
            std::lock_guard<std::mutex> lock(metaMutex);
            bounds = lastWindowBounds;
            haveBounds = hasLastWindowBounds;
        }

        if (!haveBounds)
            bounds = {};

        // Same header layout as the full atkAudio build, so saved filters stay compatible.
        s = "MULTICORE:0\n"
          + std::string("DOCKVISIBLE:")
          + (isWindowVisible() ? "1" : "0")
          + "\n"
          + "DOCKFLOATING:1\n"
          + "DOCKAREA:0\n"
          + "DOCKGEOM:"
          + std::to_string(bounds.getX())
          + ","
          + std::to_string(bounds.getY())
          + ","
          + std::to_string(bounds.getWidth())
          + ","
          + std::to_string(bounds.getHeight())
          + "\n"
          + stateString;
    }

    void setState(std::string& s)
    {
        if (s.empty())
            return;

        std::string pluginState = s;
        std::string value;
        bool restoreWindowVisible = false;

        // MULTICORE is accepted for compatibility but ignored: plugins always run on the OBS audio thread.
        readHeaderLine(pluginState, "MULTICORE:", value);

        if (readHeaderLine(pluginState, "DOCKVISIBLE:", value))
            restoreWindowVisible = (value == "1");

        readHeaderLine(pluginState, "DOCKFLOATING:", value);
        readHeaderLine(pluginState, "DOCKTAB:", value);
        readHeaderLine(pluginState, "DOCKAREA:", value);

        if (readHeaderLine(pluginState, "DOCKGEOM:", value))
        {
            int x = 0, y = 0, w = 0, h = 0;
            if (std::sscanf(value.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0)
            {
                std::lock_guard<std::mutex> lock(metaMutex);
                savedWindowPosition = {x, y};
                hasSavedWindowPosition = true;
                lastWindowBounds = {x, y, w, h};
                hasLastWindowBounds = true;
            }
        }

        {
            juce::ScopedLock lock(pendingStateLock);
            pendingStateString = pluginState;
        }

        int flags = UpdateFlags::RestoreState;
        if (restoreWindowVisible)
        {
            setWindowVisibleFlag(true);
            flags |= UpdateFlags::ShowWindow;
        }

        post(flags);
    }

    //==============================================================================
    // Stats

    int getInnerPluginChannelCount() const
    {
        auto* hostProc = hostProcessor.get();
        if (!hostProc)
            return 2;

        auto* innerPlugin = hostProc->getInnerPlugin();
        if (!innerPlugin)
            return 2;

        return juce::jmax(innerPlugin->getTotalNumInputChannels(), innerPlugin->getTotalNumOutputChannels());
    }

    float getCpuLoad() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        float currentLoad = static_cast<float>(loadMeasurer.getLoadAsProportion());

        auto now = juce::Time::getMillisecondCounterHiRes();
        if (currentLoad >= peakCpuLoad || now - peakCpuTime > 3000.0)
        {
            peakCpuLoad = currentLoad;
            peakCpuTime = now;
        }

        return peakCpuLoad;
    }

    int getLatencyMs() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        const double rate = sampleRate;
        if (rate <= 0.0)
            return 0;

        auto* hostProc = hostProcessor.get();
        if (!hostProc)
            return 0;

        auto* innerPlugin = hostProc->getInnerPlugin();
        if (!innerPlugin)
            return 0;

        const int latencySamples = innerPlugin->getLatencySamples();
        if (latencySamples <= 0)
            return 0;

        return static_cast<int>(std::round(latencySamples / rate * 1000.0));
    }

    juce::Component* getWindowComponent()
    {
        return window.get();
    }

private:
    std::atomic<int> pendingUpdateFlags{UpdateFlags::None};
    std::atomic<int> pendingChannels{0};
    std::atomic<int> pendingSamples{0};
    std::atomic<double> pendingSampleRate{0.0};
    std::string pendingStateString;
    juce::CriticalSection pendingStateLock;

    juce::CriticalSection processorLock;
    std::unique_ptr<HostAudioProcessor> hostProcessor;

    std::unique_ptr<PluginHostWindow> window;
    std::atomic<bool> windowVisible{false};
    std::atomic<bool> shuttingDown{false};

    mutable std::mutex metaMutex;
    juce::String windowTitle;
    juce::Rectangle<int> lastWindowBounds;
    bool hasLastWindowBounds = false;
    juce::Point<int> savedWindowPosition;
    bool hasSavedWindowPosition = false;

    juce::AudioBuffer<float> syncBuffer;
    juce::MidiBuffer syncMidiBuffer;

    int numChannels = 0;
    int numSamples = 0;
    std::atomic<double> sampleRate{0.0};

    std::atomic<bool> isPrepared{false};

    juce::AudioProcessLoadMeasurer loadMeasurer;

    mutable std::mutex statsMutex;
    mutable float peakCpuLoad = 0.0f;
    mutable double peakCpuTime = 0.0;

    // Keeps the shared look-and-feel alive for as long as any PluginHost exists.
    juce::SharedResourcePointer<atk::LookAndFeel> lookAndFeel;
};

void atk::PluginHost::prepare(int numChannels, int numSamples, double sampleRate)
{
    if (pImpl && numChannels > 0 && numSamples > 0 && sampleRate > 0.0)
        pImpl->requestPrepare(numChannels, numSamples, sampleRate);
}

void atk::PluginHost::process(float** buffer, int numChannels, int numSamples, double sampleRate)
{
    pImpl->process(buffer, numChannels, numSamples, sampleRate);
}

void atk::PluginHost::getState(std::string& s)
{
    pImpl->getState(s);
}

void atk::PluginHost::setState(std::string& s)
{
    pImpl->setState(s);
}

juce::Component* atk::PluginHost::getWindowComponent()
{
    return pImpl ? pImpl->getWindowComponent() : nullptr;
}

int atk::PluginHost::getInnerPluginChannelCount() const
{
    if (!pImpl)
        return 2; // Default to stereo

    return pImpl->getInnerPluginChannelCount();
}

float atk::PluginHost::getCpuLoad() const
{
    if (!pImpl)
        return 0.0f;

    return pImpl->getCpuLoad();
}

int atk::PluginHost::getLatencyMs() const
{
    if (!pImpl)
        return 0;

    return pImpl->getLatencyMs();
}

void atk::PluginHost::setVisible(bool visible)
{
    if (!pImpl)
        return;

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && mm->isThisTheMessageThread())
    {
        pImpl->setVisible(visible);
        return;
    }

    // Not on the UI thread: let the message thread do it.
    pImpl->setWindowVisibleFlag(visible);
    pImpl->post(visible ? Impl::UpdateFlags::ShowWindow : Impl::UpdateFlags::DestroyWindow);
}

void atk::PluginHost::setWindowTitle(const std::string& title)
{
    if (!pImpl)
        return;

    pImpl->setWindowTitle(juce::String::fromUTF8(title.c_str()));
}

bool atk::PluginHost::isWindowVisible() const
{
    return pImpl ? pImpl->isWindowVisible() : false;
}

atk::PluginHost::PluginHost()
{
    pImpl = std::make_unique<Impl>();
}

atk::PluginHost::~PluginHost()
{
    auto* impl = pImpl.release();
    if (impl == nullptr)
        return;

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr)
    {
        // JUCE is already gone; nothing else can safely run the teardown.
        delete impl;
        return;
    }

    if (mm->isThisTheMessageThread())
    {
        delete impl;
        return;
    }

    // OBS destroys sources on its own destruction thread. The plugin, its editor and the window
    // must be torn down on the message (UI) thread, so hand the deletion over to it.
    if (!juce::MessageManager::callAsync([impl]() { delete impl; }))
        atk::logging::warning("PluginHost", "could not schedule PluginHost teardown; instance leaked");
}
