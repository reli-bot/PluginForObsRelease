/*
This file is part of the atkAudio plugin for OBS.
It is distributed under the AGPLv3 license. See the LICENSE file for details.

The "VST 3.x Eklentisi" audio filter: one VST3 plugin per OBS filter.
*/

#include "core/atkaudio/Logging.h"
#include "core/atkaudio/PluginHost/PluginHost.h"
#include "core/atkaudio/atkaudio.h"

#include <obs-module.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Name in OBS' filter list (OBS' own VST2 filter is "VST 2.x Eklentisi" in Turkish).
#define FILTER_DISPLAY_NAME "VST 3.x Eklentisi"
// Unchanged from atkAudio PluginHost, so saved filters keep working.
#define FILTER_ID "atkaudio_plugin_host"

#define OPEN_PLUGIN_SETTINGS "open_plugin_settings"
#define CLOSE_PLUGIN_SETTINGS "close_plugin_settings"

struct pluginhost_data
{
    obs_source_t* context = nullptr;

    std::unique_ptr<atk::PluginHost> pluginHost;

    // The hosted processor has a sidechain input bus next to the main bus (as in atkAudio);
    // in this VST3-only build it always receives silence.
    std::vector<std::vector<float>> silentSidechain;
    std::vector<float*> pointersToProcess;

    size_t num_channels = 2;
    size_t sample_rate = 48000;

    bool rename_signal_connected = false;
    bool hasLoadedState = false;
};

/* -------------------------------------------------------- */

static const char* pluginhost_name(void* unused)
{
    UNUSED_PARAMETER(unused);
    return FILTER_DISPLAY_NAME;
}

static void on_filter_rename(void* data, calldata_t* calldata)
{
    auto* ph = (struct pluginhost_data*)data;
    UNUSED_PARAMETER(calldata);

    if (!ph || !ph->context || !ph->pluginHost)
        return;

    const char* filterName = obs_source_get_name(ph->context);
    if (!filterName || !*filterName)
        return;

    ph->pluginHost->setWindowTitle(filterName);
}

static void save(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost_data*)data;

    std::string s;
    ph->pluginHost->getState(s);

    atk::logging::debug("OBS_API.PluginHost.save", "stateSize=" + juce::String((int)s.size()));

    obs_data_set_string(settings, FILTER_ID, s.c_str());
}

static void load(void* data, obs_data_t* settings)
{
    auto* ph = (struct pluginhost_data*)data;

    if (ph->hasLoadedState)
        return;

    ph->hasLoadedState = true;

    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    std::string stateStr = chunkData ? chunkData : "";

    atk::logging::debug("OBS_API.PluginHost.load", "stateSize=" + juce::String((int)stateStr.size()));

    ph->pluginHost->setState(stateStr);
}

static void pluginhost_update(void* data, obs_data_t* settings)
{
    UNUSED_PARAMETER(settings);

    auto* ph = (struct pluginhost_data*)data;
    ph->num_channels = audio_output_get_channels(obs_get_audio());
    ph->sample_rate = audio_output_get_sample_rate(obs_get_audio());
}

static void* pluginhost_create(obs_data_t* settings, obs_source_t* filter)
{
    if (!atk::isReady() || atk::isShuttingDown())
    {
        atk::logging::warning("OBS_API.PluginHost.create", "skipping create because lifecycle is not ready");
        return nullptr;
    }

    auto* ph = new pluginhost_data();
    ph->context = filter;
    ph->pluginHost = std::make_unique<atk::PluginHost>();

    const char* filterName = obs_source_get_name(filter);
    if (filterName && *filterName)
        ph->pluginHost->setWindowTitle(filterName);

    if (signal_handler_t* signalHandler = obs_source_get_signal_handler(filter))
    {
        signal_handler_connect(signalHandler, "rename", on_filter_rename, ph);
        ph->rename_signal_connected = true;
    }

    pluginhost_update(ph, settings);

    ph->pointersToProcess.resize(ph->num_channels * 2, nullptr);
    ph->silentSidechain.assign(ph->num_channels, std::vector<float>(AUDIO_OUTPUT_FRAMES, 0.0f));

    // Prepare the processor before the first audio callback, so a saved plugin is instantiated
    // even if the parent source is not producing audio yet.
    ph->pluginHost->prepare((int)ph->num_channels, AUDIO_OUTPUT_FRAMES, (double)ph->sample_rate);

    // Restore the saved state here (OBS' load callback is not called for every kind of source).
    const char* chunkData = obs_data_get_string(settings, FILTER_ID);
    if (chunkData && *chunkData)
    {
        std::string stateStr = chunkData;
        ph->pluginHost->setState(stateStr);
        ph->hasLoadedState = true;
    }

    return ph;
}

static void pluginhost_destroy(void* data)
{
    auto* ph = (struct pluginhost_data*)data;

    if (ph->rename_signal_connected && ph->context)
    {
        if (signal_handler_t* signalHandler = obs_source_get_signal_handler(ph->context))
            signal_handler_disconnect(signalHandler, "rename", on_filter_rename, ph);
    }

    ph->pluginHost.reset();
    delete ph;
}

static struct obs_audio_data* pluginhost_filter_audio(void* data, struct obs_audio_data* audio)
{
    auto* ph = (struct pluginhost_data*)data;

    const int numSamples = (int)audio->frames;
    if (numSamples == 0)
        return audio;

    const int numChannels = (int)ph->num_channels;
    auto** samples = (float**)audio->data;

    // Main bus: OBS' own buffers (processed in place). Sidechain bus: zeros, refreshed every block
    // because the routing also writes the plugin's extra outputs there.
    if ((int)ph->silentSidechain.size() != numChannels)
        ph->silentSidechain.resize(numChannels);

    for (auto& channel : ph->silentSidechain)
    {
        if ((int)channel.size() < numSamples)
            channel.resize(numSamples);
        std::fill_n(channel.begin(), numSamples, 0.0f);
    }

    ph->pointersToProcess.resize(numChannels * 2);
    for (int i = 0; i < numChannels; ++i)
    {
        ph->pointersToProcess[i] = samples[i];
        ph->pointersToProcess[i + numChannels] = ph->silentSidechain[i].data();
    }

    ph->pluginHost->process(ph->pointersToProcess.data(), numChannels, numSamples, (double)ph->sample_rate);

    return audio;
}

static bool open_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    UNUSED_PARAMETER(property);

    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), false);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), true);

    auto* ph = (struct pluginhost_data*)data;
    ph->pluginHost->setVisible(true);

    return true;
}

static bool close_editor_button_clicked(obs_properties_t* props, obs_property_t* property, void* data)
{
    UNUSED_PARAMETER(property);

    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), true);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), false);

    auto* ph = (struct pluginhost_data*)data;
    ph->pluginHost->setVisible(false);

    return true;
}

static obs_properties_t* pluginhost_properties(void* data)
{
    auto* ph = (struct pluginhost_data*)data;
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_button2(
        props,
        OPEN_PLUGIN_SETTINGS,
        obs_module_text("OpenPluginInterface"),
        open_editor_button_clicked,
        data
    );
    obs_properties_add_button2(
        props,
        CLOSE_PLUGIN_SETTINGS,
        obs_module_text("ClosePluginInterface"),
        close_editor_button_clicked,
        data
    );

    const bool windowOpen = ph != nullptr && ph->pluginHost != nullptr && ph->pluginHost->isWindowVisible();
    obs_property_set_visible(obs_properties_get(props, OPEN_PLUGIN_SETTINGS), !windowOpen);
    obs_property_set_visible(obs_properties_get(props, CLOSE_PLUGIN_SETTINGS), windowOpen);

    return props;
}

struct obs_source_info pluginhost_filter = {
    .id = FILTER_ID,
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = pluginhost_name,
    .create = pluginhost_create,
    .destroy = pluginhost_destroy,
    .get_properties = pluginhost_properties,
    .update = pluginhost_update,
    .filter_audio = pluginhost_filter_audio,
    .save = save,
    .load = load,
};
