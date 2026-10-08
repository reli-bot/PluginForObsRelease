# atkAudio PluginHost for OBS — VST3-only build for OBS 31.0.4

A stripped-down fork of [atkAudio Plugin for OBS](https://github.com/atkAudio/PluginForObsRelease)
(0.34.8) that contains only the **atkAudio PluginHost** audio filter: one VST3 plugin per OBS
filter. It is built against **OBS Studio 31.0.4**.

## What is left

- "atkAudio PluginHost" audio filter, hosting one VST3 plugin (Windows, 64-bit)
- Plugin window opens from the filter's **Open Plugin Settings** button, as its own window
  owned by the OBS main window (position and open/closed state are saved with the filter)
- Optional sidechain input (filter property "Sidechain")
- Plugin list is read from a cache; scanning happens only when you choose
  **Options… → Scan for new or updated VST3 plug-ins**, each file in the separate
  `atkaudio-pluginforobs_scanner.exe` process
- Filter state format is the same as upstream PluginHost

## What was removed

PluginHost2, DeviceIo / DeviceIo2, Audio Source Mixer, Delay filter, MIDI control (Tools menu),
audio/MIDI device routing and multithreading in PluginHost, the online update check, and
LV2/LADSPA/AU/ARA hosting. There is **no Qt dependency** any more, so the module does not depend
on the Qt version shipped with OBS (the upstream 0.34.8 release needs Qt 6.8 and OBS 31.1.1 and
does not load in OBS 31.0.4, which ships Qt 6.6.3). The only OBS libraries used are `obs.dll`
and `obs-frontend-api.dll` (plain C APIs).

## Install (Windows)

1. Close OBS and remove any other atkAudio installation.
2. Copy the `atkaudio-pluginforobs` folder (with `bin\64bit\atkaudio-pluginforobs.dll`,
   `bin\64bit\atkaudio-pluginforobs_scanner.exe` and `data\`) to
   `C:\ProgramData\obs-studio\plugins\`.
3. Start OBS, add the **atkAudio PluginHost** filter to an audio source, click
   **Open Plugin Settings**, scan once via **Options…**, select a plugin and **Load plugin**.

## Build

The GitHub Actions workflow `.github/workflows/build-windows.yml` builds the module on
`windows-2022` (Visual Studio 2022) and then runs it in a real OBS 31.0.4 with two test VST3
plugins (`tests/`).

Locally (Visual Studio 2022, CMake ≥ 3.28):

```console
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix package
```

Only OBS headers are needed (fetched automatically, or pass `-DOBS_SOURCE_DIR=<obs-studio 31.0.4>`);
the import libraries are generated from the export lists of the official OBS 31.0.4 DLLs in
`cmake/obs-sdk/31.0.4/`.

## License

AGPLv3, see [LICENSE](LICENSE). Original work © atkAudio.
