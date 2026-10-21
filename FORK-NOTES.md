# Fork notes

Every place this fork touches upstream AetherSDR code outside
`src/core/backends/orion/`. Keep this list short; each entry is a
possible merge conflict when pulling from `upstream`.

| File | Change | Why |
|---|---|---|
| `CLAUDE.md` | Fork rules and pointers added above the upstream text | Fork rules load first in every Claude Code session |
| `CMakeLists.txt` | One line, `include(cmake/AetherOrion.cmake)`, after the `Qt6SerialPort_FOUND` block | Every fork build rule lives in that one file |
| `CMakeLists.txt` | One `if(COMMAND aether_orion_patch_setup_page_targets)` call right after `include(tests/tests.cmake)` | `RadioSetupDialog.cpp` calls `buildOrionSetupPage()`, so every test target that compiles that dialog needs the page's definition; one call beats editing a dozen test targets |
| `src/models/RadioModel.cpp` | `#include` of `OrionBackend.h` and one `orion` branch in `makeBackend()`, both under `AETHER_BACKEND_ORION` | The factory is the only place a family string picks a backend |
| `src/models/RadioModel.cpp` | In `publishCapabilities()`: a backend with no command plane that declares `hasSelectableMicInputs` gets its mic-source list from `capabilities().extensions["micInputs"]` | The Phone applet's MIC/PC choice; upstream fills that list only from the Flex `mic list` command. No family check |
| `src/gui/RadioSetupDialog.cpp` | Under `AETHER_BACKEND_ORION`: the include of `gui/orion/OrionSetupPage.h` and an "Orion" page in the Radio category, shown while a backend declaring the `orion` namespace is connected | The Orion's phones/speaker routing (`*UC`) and CW skimmer switches need a place in AetherSDR's own settings; the page itself lives in the fork's file |
| `src/gui/MainWindow_Spots.cpp` | Under `AETHER_BACKEND_ORION`, next to the RBN wiring: a listener re-armed on each connect that turns the backend's `orion` / `skimmer.spot` extension events into `DxSpot`s (source "Skimmer", violet, 10 min) through the same `queueSpotCmd` as RBN; plus three includes | The skimmer runs on the RSP2's IQ inside the backend; its finds join the panadapter like any spot source |
| `src/models/RadioModel.cpp` | In `setPcAudioEnabled()` and `notePcAudioEnabled()`, under `AETHER_BACKEND_ORION`: a backend declaring the `orion` namespace gets `audio.pc` / `audio.pc.state` | PC Audio decides whether slice volume and mute drive the RSP2 audio or the Orion's own AF (`*UM`/`*US`); upstream passes it to the `icom` namespace only |
| `src/gui/ConnectionPanel.h` | `kFamilyOrion` constant; `connectSerialRadio()` / `isSerialRadioSerial()` | Same pattern as the other families; startup auto-connect for serial radios |
| `src/gui/ConnectionPanel.cpp` | Family normalisation (two places), the "Ten-Tec Orion (serial)" radio-type entry, the hint and placeholder text, and an `orion` branch at the top of `probeRadio()` that hands the device path to the session as `orion:<path>` and remembers it (`SerialDevice/<family>`); the serial-family table (`kSerialRadioFamilies`, Omni VII joins it at step 5) that fills the device box; `connectSerialRadio()` | The manual-connect page is how the operator picks the radio; a serial radio has nothing to probe |
| `src/gui/MainWindow.cpp` | In the startup auto-connect block, under `AETHER_BACKEND_ORION`: a saved `<family>:/dev/...` serial connects directly via `connectSerialRadio()` | "Connect to last radio on start" waits for discovery or a routed IP, which a serial radio never gives |
| `src/gui/MainWindow_Menus.cpp` | Under `AETHER_ORION_LOGBOOK`: the include of `gui/orion/OrionLogbookUi.h` and one call, `installOrionLogbook(...)`, right after the Tools menu is created; under `AETHER_BACKEND_ORION`: the include of `gui/orion/OrionSkimmerUi.h` and one call, `installOrionSkimmerWindows(...)`, adding two Tools items | The station logbook's "Logbook" menu sits between Tools and View; the skimmer's band map and waterfall open from Tools |
| `src/gui/MainWindow_Wiring.cpp` | Under `AETHER_ORION_LOGBOOK`: the include of `gui/orion/OrionLogbookUi.h` and, in the spot-marker rebuild, `orionWorkedSpotColour()` when the DXCC colouring gave no colour | Spots coloured by worked-before status from the station log (red new, amber new band, green worked, grey confirmed) |
| `src/gui/PhoneCwApplet.cpp` | `setSelectableMicInputs()` uses the model's mic-input list when it has one, instead of always FlexRadio's MIC/BAL/LINE/ACC/PC | Shows the Orion's MIC and PC; a Flex is unchanged (its list comes from `mic list`) |
| `src/core/backends/IRadioBackend.h` | `setPanWaterfallRate()` and `setPanWaterfallAverage()`, both default no-ops | The waterfall gets its own copy of each frame from this backend, so it needs its own row interval and its own average; every other backend ignores them |
| `src/core/backends/RadioCapabilities.h` | One field, `backendPanWaterfallAveraging` | Declares that the waterfall is averaged separately from the trace; the Display panel's WF AVG row is shown only when it is present |
| `src/models/RadioModel.cpp` | In `requestPanDisplayRates()`: the waterfall rate also goes to the backend; plus `requestPanWaterfallAverage()` | The rows are still paced in RadioModel, but a backend shaping its own waterfall copy has to know the rate and the average |
| `src/gui/SpectrumOverlayMenu.cpp/.h` | A "WF AVG:" row at the top of the WATERFALL group, `wfAverageChanged`, `setWaterfallAverageAvailable()`, `syncWaterfallAverage()` | Separate controls for the trace and the waterfall: FFT AVG smooths the trace, WF AVG the waterfall. Hidden on every radio that does not declare it |
| `src/gui/MainWindow_Wiring.cpp` | The WF AVG connection, its availability, and its per-radio/per-pan persistence (`DisplayWfAverage/...`) | Nothing echoes the setting back, so the client owns it |
| `src/gui/MainWindow_Shortcuts.cpp` | In `toggleCwKeyerPanel()`, under `AETHER_BACKEND_ORION`: on the `orion` family the CWX indicator opens the fork's CW window instead of the CWX panel (setting `OrionCw/replaceCwx`) | Fork rule 2's CW exception: the station's CW window is the console's, on AetherSDR's keying |
| `src/gui/MainWindow_Menus.cpp` | Under `AETHER_BACKEND_ORION`: the include of `gui/orion/OrionCwUi.h` and one call, `installOrionCwWindow(...)`, adding a Tools item | Tools -> CW window... (Ctrl+Shift+W) |
| `src/models/CwRxModel.cpp` | Under `AETHER_BACKEND_ORION`: `orion-fldigi` and `orion-bayes` in `availableBackends()` and `makeBackend()`, plus one include | The Orion console's two CW readers join AetherSDR's own in the reader's decoder list |
| `src/gui/AppletPanel.cpp/.h` | Under `AETHER_BACKEND_ORION`: the `ROT` applet entry (the DCU-3 rotor tile) beside `GHE`, its bar-order slot and its palette category | The operator asked for the rotor on the right-hand panel, in the same shape as the Green Heron tile |
| `src/gui/MainWindow.cpp` | Under `AETHER_BACKEND_ORION`: hands the rotor tile the radio model after the panel is built | The panel is built before any radio exists |
| `src/gui/SpectrumWidget.cpp/.h` | Three colour schemes appended to `WfColorScheme` (Enhanced, Classic, Thermal) with their names and theme tokens | The console's palettes, offered beside AetherSDR's own. Appended, never reordered: the scheme is stored by index |
| `resources/themes/default-dark.json`, `src/core/ThemeSeedGenerated.cpp` | Three colormaps (`enhanced`, `classic`, `thermal`) | The palettes above; regenerated with `tools/gen_theme_seed.py` |
| `src/gui/MainWindow_Wiring.cpp` | `requestZeroBeatForSlice()` extracted from the VfoWidget lambda | The CW window's 0-BEAT button and the VFO box's run one implementation |
| `src/gui/MainWindow_Menus.cpp` | Two callbacks passed to `installOrionLogbook()`: a DX-cluster spot through `m_dxCluster->sendCommand()`, and opening AetherMap | The Log QSO window's Spot and Globe buttons; both belong to the host, not the window |
| `src/core/backends/orion/OrionCat.*` | `*CT` / `*CQ` / `*CD` and the `?CT ?CQ ?CD ?CK ?CS ?CW` queries, with a `CwRigReply` | The radio's own CW settings, which apply whatever does the keying |
| `tests/tests.cmake` | `orion_cat_test`, `orion_spectrum_test`, `orion_rxaudio_test`, `orion_peripherals_test`, `orion_skimmer_test`, `orion_logbook_test`, `orion_upload_test` and `orion_backend_test` appended at the end | Upstream requires every test target to be declared there |
| `tests/backend_seam_affinity_test.cpp` | `orion` added to the constructed-families list | The seam contract test covers the new backend too |

New files that upstream does not have (no merge risk):

- `FORK-NOTES.md` (this file)
- `cmake/AetherOrion.cmake`
- `docs/architecture/orion-backend-design.md`
- `src/core/backends/orion/` (`OrionBackend`, `OrionCat`, `OrionPollScheduler`,
  `OrionTxAudio`, the station peripherals `OrionWinKeyer`, `OrionRotor`
  (with `OrionRotctld`), `OrionCwDaemon`, `OrionPatMonitor`, the CW
  skimmer glue `OrionSkimmer` and `OrionSkimmerFeed`, and for the RSP2
  `Rsp2Receiver`, `OrionSpectrum`, `OrionRxAudio`, `SdrPlayApi`)
- `src/core/backends/orion/skimmer/` (`SkimmerEngine`, `CwDecoder`,
  `FldigiCwEngine`, `BayesCwEngine`, `SkimServer`, `SkimStft`: ported from
  the Orion console, GPL-2.0-or-later, namespace and include paths only,
  plus the engine watching the LO instead of the dial and `SkimStft` on
  FFTW; built with OpenMP when found)
- `src/core/backends/orion/log/` (`LogDb`, `Adif`, `CqrlogSync`,
  `LogbookIndex`, `WsjtxListener`, `QslUploader`: ported from the Orion console,
  GPL-2.0-or-later, with AetherSDR settings and cty.dat; plus the glue
  `OrionLogbook`, and `OrionUploadSecrets` (upload secrets in the system
  keychain); needs Qt6::Sql)
- `src/core/backends/orion/OrionCwRxBackend.cpp/.h` (the console's CW
  readers as AetherSDR `CwRxBackend`s, over the skimmer's `skim::CwDecoder`)
- `src/gui/orion/OrionRotorApplet.cpp/.h` (the DCU-3 rotor tile)
- `src/gui/orion/OrionBrowserGlobe.cpp/.h` and `resources/orion-globe/`
  (the console's browser globe, ported whole, with its own assets so the
  page needs no network; see THIRD_PARTY_LICENSES 32 and 33)
- `src/core/backends/orion/CwMacros.cpp/.h` (the console's plain-language CW
  macro vocabulary, ported: namespace and include paths only)
- `src/core/backends/orion/log/QsoContext.h` (the console's shared
  current-QSO record, ported: the Log QSO window and the CW window are
  about one station)
- `src/gui/orion/OrionCwWindow.cpp/.h` and `src/gui/orion/OrionCwUi.cpp/.h`
  (the CW window and its Tools entry)
- `src/gui/orion/OrionSetupPage.cpp/.h` (Radio Setup -> Orion),
  `src/gui/orion/OrionLogbookUi.cpp/.h` (the Logbook menu, Log QSO window
  and logbook browser) and `src/gui/orion/OrionSkimmerUi.cpp/.h` (the
  skimmer band map and waterfall); all built into the AetherSDR target from
  `cmake/AetherOrion.cmake`
- `tests/orion_cat_test.cpp`, `tests/orion_backend_test.cpp`,
  `tests/orion_spectrum_test.cpp`, `tests/orion_rxaudio_test.cpp`,
  `tests/orion_peripherals_test.cpp`, `tests/orion_skimmer_test.cpp`,
  `tests/orion_logbook_test.cpp`, `tests/orion_upload_test.cpp`
- `tests/orion/fake_orion.py`

## Running against the fake radio

```bash
python3 tests/orion/fake_orion.py /tmp/fake-orion
```

Then, in AetherSDR's connection panel, choose **Ten-Tec Orion (serial)**
as the radio type, enter `/tmp/fake-orion` and connect. For the real radio
enter `/dev/orion`, with the Orion console closed (fork rule 7).

## The SDRplay library is never bundled (fork rule 5)

The RSP2 code needs the SDRplay API **headers** at build time (from the
operator's API install, never vendored) and loads `libsdrplay_api` with
`dlopen()` at run time. Nothing links it, so no AppImage or installer can
pick it up. Check after packaging changes:

```bash
ldd build/AetherSDR | grep -i sdrplay
```

It must print nothing.

## Station peripherals start with the Orion session

Connecting the Orion also opens the WinKeyer (FTDI `A904QF5Z`), the DCU-3
(`/dev/portS4`), rotctld on `localhost:4533` and cwdaemon on
`localhost:6789`, and watches Pat. Disconnecting releases all of them. The
Orion console must not be running (fork rule 7), and `rotctld.service`
stays retired. Each can be switched off for a session with a connect option
(`orion.winkeyer.device`, `orion.rotor.device`, `orion.cwdaemon.port`,
`orion.pat.url` = `none`); the tests use these to stay off the hardware.
| `src/models/RadioModel.cpp/.h` | `setPanBackendAverageOverride()`, and `requestPanAverage()` sends the override to the backend where one is set | The Orion Console mode averages its own trace, so the backend hands it raw frames; the pan model keeps FFT AVG, which returns when the override clears. 2D and 3D are fed as upstream feeds them |
| `src/gui/MainWindow_Wiring.cpp`, `src/gui/MainWindow_Session.cpp` | `syncBackendAverageToRenderMode()`, called on the Display panel's mode change, on pan display restore, on pan clone and on the startup mode restore | Same reason |
| `src/gui/SpectrumWidget.cpp/.h` | `SpectrumRenderMode::ModeOrion` (appended), the `m_orionPan` renderer built on first pick, `syncOrionPanSettings()`, the frame tap in `updateSpectrum()`, the gated spectrum/waterfall draws on both paint paths (two textures beside the 3DSS fallback on the GPU path), the overlay grid skipped in that mode, and the left-edge strips in the mouse handlers | Fork rule 2: the Orion console's panadapter as a third Display mode, drawn by `gui/orion/OrionPanRenderer` from this widget's own window, scheme, FFT AVG and waterfall rate; the waterfall's reference (`OrionPanWfRefDb`) is the one setting of its own |
| `src/gui/SpectrumOverlayMenu.cpp` | A third item, "Orion Console", in the Spectrum combo | The mode is picked where 2D and 3D are |
| `cmake/AetherOrion.cmake` | `src/gui/orion/OrionPanRenderer.cpp/.h` | The renderer, fork-only |
| `src/gui/MainWindow_Wiring.cpp`, `src/gui/SpectrumOverlayMenu.cpp/.h` | On the Spectrum combo's change to 3D: `setDssFloorDepth(6)`, `resetDisplay3DSettings()`, the menu's `syncDisplay3D(6, 70, 100)`, and FFT AVG set to 0 through the normal path | Picking 3D opens it as a fresh install would, whatever the sliders drifted to |
