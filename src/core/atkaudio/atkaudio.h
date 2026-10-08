#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

// Project-wide debug-print macro, decoupled from JUCE's own DBG (which is a no-op
// outside JUCE_DEBUG, and otherwise routes through OutputDebugString rather than
// std::cerr). Enabled in local Debug/RelWithDebInfo builds only.
#if defined(ATK_DEBUG) && !defined(ATK_CI_BUILD)
#define ATK_DBG(x) std::cerr << x << std::endl
#else
#define ATK_DBG(x)
#endif

namespace atk
{
// Initialise the JUCE runtime on the calling thread (the OBS UI thread), which becomes
// the JUCE message thread. On Windows the OBS (Qt) event loop dispatches JUCE's window
// messages, so no extra pump is needed.
bool create();

// Destroy every remaining PluginHost instance and shut the JUCE runtime down.
void destroy();

// Dispatch pending JUCE messages (only valid on the message thread).
void pump();

// Returns true when the OBS JUCE format lifecycle is fully initialized.
bool isReady();

// Returns true while lifecycle shutdown is in progress.
bool isShuttingDown();

// Native handle of the OBS main window (HWND on Windows), or nullptr.
void* getObsMainWindowHandle();

// Live PluginHost instance bookkeeping, so shutdown can wait for deferred deletions.
void registerHostInstance();
void unregisterHostInstance();

// Return the settings file for the given name under the OBS config dir. This requires a live OBS module context and
// intentionally has no fallback path.
juce::File getSettingsFile(const juce::String& name);

} // namespace atk
