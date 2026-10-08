# VST 3.x Eklentisi — VST3-only atkAudio PluginHost for OBS 31.0.4

A stripped-down fork of [atkAudio Plugin for OBS](https://github.com/atkAudio/PluginForObsRelease)
(0.34.8). It contains a single OBS audio filter, shown in OBS as **"VST 3.x Eklentisi"**, that
hosts one VST3 plugin per filter. It is built against **OBS Studio 31.0.4**.

## What it does

- One VST3 plugin per filter (Windows, 64-bit). Chain several filters for a plugin chain.
- **Eklenti Arabirimi Aç / Kapat** (Open / Close Plug-in Interface) opens the plugin in its own
  window, owned by the OBS main window. Window position and open/closed state are saved with
  the filter.
- The plugin list is read from a cache. Plugins are scanned only when you choose
  **Options… → Scan for new or updated VST3 plug-ins**; each file is scanned in the separate
  `atkaudio-pluginforobs_scanner.exe` process, so a broken plugin cannot take OBS down.
- Same filter id and state format as upstream PluginHost.

## No network ("closed box")

The update check is gone and JUCE's networking is compiled out
(`cmake/patches/juce-no-network.patch`, `JUCE_ATK_NO_NETWORK`): sockets, the HTTP client behind
`URL`/`WebInputStream` and network-adapter queries are replaced by stubs that always fail, and no
networking library is linked. Every CI build checks that the DLL and the scanner neither import,
delay-load nor name WinINet, WinSock, WinHTTP, URLMon, IP Helper or similar libraries
(`tests/ci/check_no_network.py`). VST3 plugins you load are separate programs and keep their own
behaviour (for example licence checks).

## What was removed

PluginHost2, DeviceIo / DeviceIo2, Audio Source Mixer, Delay filter, MIDI control (Tools menu),
audio/MIDI device routing, multithreading, sidechain input, the online update check, all
networking, LV2/LADSPA/AU/ARA hosting, and Qt. Without Qt the module does not depend on the Qt
version OBS ships (the upstream 0.34.8 release needs Qt 6.8 and OBS 31.1.1 and does not load in
OBS 31.0.4, which ships Qt 6.6.3). The only OBS libraries used are `obs.dll` and
`obs-frontend-api.dll` (plain C APIs).

## Install (Windows)

1. Close OBS and remove any other atkAudio installation.
2. Copy the `atkaudio-pluginforobs` folder (with `bin\64bit\atkaudio-pluginforobs.dll`,
   `bin\64bit\atkaudio-pluginforobs_scanner.exe` and `data\`) to
   `C:\ProgramData\obs-studio\plugins\`.
3. Start OBS, add the **VST 3.x Eklentisi** filter to an audio source, click
   **Eklenti Arabirimi Aç**, scan once via **Options…**, select a plugin and click **Load plugin**.

## Build

`.github/workflows/build-windows.yml` builds on `windows-2022` (Visual Studio 2022), checks the
binaries for network code, then runs them in a real OBS 31.0.4 with two test VST3 plugins
(`tests/`) and publishes the result as a pre-release of this fork.

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
