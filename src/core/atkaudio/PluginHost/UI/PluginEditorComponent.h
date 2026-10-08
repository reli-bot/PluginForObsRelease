#pragma once

#include "../Core/HostAudioProcessor.h"
#include "PluginHostFooter.h"
#include "UICommon.h"

#include <atkaudio/Logging.h>
#include <cmath>
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

class PluginEditorComponent final : public juce::Component
{
public:
    ~PluginEditorComponent() override
    {
        if (editor != nullptr && editor->getParentComponent() == this)
            removeChildComponent(editor.get());
    }

    template <typename Callback>
    PluginEditorComponent(
        std::unique_ptr<juce::AudioProcessorEditor> editorIn,
        HostAudioProcessorImpl* processorPtr,
        Callback&& onClose
    )
        : editor(std::move(editorIn))
        , processor(processorPtr)
        , footer("Unload Plugin", true)
    {
        addAndMakeVisible(editor.get());
        addAndMakeVisible(footer);

        childBoundsChanged(editor.get());

        auto lambda = [onClose]
        {
            juce::AlertWindow::showOkCancelBox(
                juce::AlertWindow::WarningIcon,
                "Unload Plugin",
                "Are you sure you want to unload the plugin?",
                "Yes",
                "No",
                nullptr,
                juce::ModalCallbackFunction::create(
                    [onClose](int result)
                    {
                        if (result == 1)
                            onClose();
                    }
                )
            );
        };
        footer.actionButton.onClick = lambda;

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

        // Initialize cached scale factor for DPI change detection
        if (auto* peer = getPeer())
            cachedScaleFactor = (float)peer->getPlatformScaleFactor();
    }

    juce::AudioProcessorEditor* getEditor() const
    {
        return editor.get();
    }

    void setFooterVisible(bool visible)
    {
        if (footer.isVisible() == visible)
            return;

        footer.setVisible(visible);

        // Recalculate size based on footer visibility
        if (editor != nullptr)
        {
            const auto editorSize = editor->getLocalBounds();
            int totalHeight = editorSize.getHeight();
            if (visible)
                totalHeight += margin + buttonHeight;
            const juce::ScopedValueSetter<bool> guard(resizingFromChild, true);
            setSize(editorSize.getWidth(), totalHeight);

            resized();
            repaint();
        }
    }

    juce::ComponentBoundsConstrainer* getEditorConstrainer() const
    {
        if (editor != nullptr)
        {
            auto* c = editor->getConstrainer();
            if (c != nullptr)
            {
                if (c->getMinimumWidth() <= 0 || c->getMinimumHeight() <= 0)
                    c->setMinimumSize(editor->getWidth(), editor->getHeight());
                return c;
            }
        }
        return nullptr;
    }

    void resized() override
    {
        auto bounds = getLocalBounds();

        int footerSpace = footer.isVisible() ? (buttonHeight + margin) : 0;
        auto editorBounds = bounds.removeFromTop(bounds.getHeight() - footerSpace);

        if (footer.isVisible())
        {
            auto footerBounds = bounds.withTrimmedTop(margin);
            footer.setBounds(footerBounds.reduced(margin, 0).withTrimmedBottom(margin));
        }

        if (editor != nullptr && !resizingFromChild && editor->getBounds() != editorBounds)
        {
            const juce::ScopedValueSetter<bool> guard(resizingFromChild, true);
            editor->setBounds(editorBounds);
        }

        checkScaleFactorChanged();
    }

    void moved() override
    {
        juce::Component::moved();
        checkScaleFactorChanged();
    }

    void childBoundsChanged(juce::Component* child) override
    {
        if (child != editor.get() || resizingFromChild)
            return;

        const auto size = editor != nullptr ? editor->getLocalBounds() : juce::Rectangle<int>();

        int totalHeight = size.getHeight();
        if (footer.isVisible())
            totalHeight += margin + buttonHeight;

        if (size.getWidth() == getWidth() && totalHeight == getHeight())
            return;

        const juce::ScopedValueSetter<bool> guard(resizingFromChild, true);
        setSize(size.getWidth(), totalHeight);
    }

private:
    static constexpr auto buttonHeight = 54;

    HostAudioProcessorImpl* processor = nullptr;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    PluginHostFooter footer;
    bool resizingFromChild = false;
    float cachedScaleFactor = 0.0f;

    // Detect DPI changes when the component is moved or resized.
    // Child windows don't receive WM_DPICHANGED, so we check on movement.
    // When scale changes, we recreate the plugin editor to render at the new DPI.
    void checkScaleFactorChanged()
    {
        if (editor == nullptr || processor == nullptr)
            return;

        if (auto* peer = getPeer())
        {
            float currentScale = (float)peer->getPlatformScaleFactor();
            if (std::abs(currentScale - cachedScaleFactor) > 0.01f)
            {
                atk::logging::info(
                    "PluginEditorComponent::checkScaleFactorChanged",
                    juce::String::formatted(
                        "scale factor changed %.2f -> %.2f; scheduling editor recreation",
                        cachedScaleFactor,
                        currentScale
                    )
                );
                // Defer editor recreation to avoid re-entrancy during resize callbacks
                juce::MessageManager::callAsync(
                    [safeThis = juce::Component::SafePointer<PluginEditorComponent>(this)]
                    {
                        if (safeThis != nullptr)
                            safeThis->recreateEditor();
                    }
                );
            }
        }
    }

    void recreateEditor()
    {
        if (processor == nullptr)
            return;

        // Verify scale actually changed (prevents unnecessary recreation from stale/duplicate async
        // calls)
        float newScale = 0.0f;
        if (auto* peer = getPeer())
        {
            newScale = (float)peer->getPlatformScaleFactor();
            if (std::abs(newScale - cachedScaleFactor) < 0.01f)
                return; // Scale hasn't changed, skip recreation
        }

        cachedScaleFactor = newScale;

        // Remove old editor
        if (editor != nullptr)
        {
            removeChildComponent(editor.get());
            if (auto* audioProc = editor->getAudioProcessor())
                audioProc->editorBeingDeleted(editor.get());
            editor.reset();
        }

        // Create new editor
        editor = processor->createInnerEditor();
        if (editor != nullptr)
        {
            addAndMakeVisible(editor.get());
            childBoundsChanged(editor.get());
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorComponent)
};
