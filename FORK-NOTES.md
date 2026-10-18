# Fork notes

Every place this fork touches upstream AetherSDR code outside
`src/core/backends/orion/`. Keep this list short; each entry is a
possible merge conflict when pulling from `upstream`.

| File | Change | Why |
|---|---|---|
| `CLAUDE.md` | Fork rules and pointers added above the upstream text | Fork rules load first in every Claude Code session |
| `CMakeLists.txt` | One line, `include(cmake/AetherOrion.cmake)`, after the `Qt6SerialPort_FOUND` block | Every fork build rule lives in that one file |
| `src/models/RadioModel.cpp` | `#include` of `OrionBackend.h` and one `orion` branch in `makeBackend()`, both under `AETHER_BACKEND_ORION` | The factory is the only place a family string picks a backend |
| `src/models/RadioModel.cpp` | In `publishCapabilities()`: a backend with no command plane that declares `hasSelectableMicInputs` gets its mic-source list from `capabilities().extensions["micInputs"]` | The Phone applet's MIC/PC choice; upstream fills that list only from the Flex `mic list` command. No family check |
| `src/gui/RadioSetupDialog.cpp` | Under `AETHER_BACKEND_ORION`: the include of `gui/orion/OrionSetupPage.h` and an "Orion" page in the Radio category, shown while a backend declaring the `orion` namespace is connected | The Orion's phones/speaker routing (`*UC`) and CW skimmer switches need a place in AetherSDR's own settings; the page itself lives in the fork's file |
| `src/gui/MainWindow_Spots.cpp` | Under `AETHER_BACKEND_ORION`, next to the RBN wiring: a listener re-armed on each connect that turns the backend's `orion` / `skimmer.spot` extension events into `DxSpot`s (source "Skimmer", violet, 10 min) through the same `queueSpotCmd` as RBN; plus three includes | The skimmer runs on the RSP2's IQ inside the backend; its finds join the panadapter like any spot source |
| `src/models/RadioModel.cpp` | In `setPcAudioEnabled()` and `notePcAudioEnabled()`, under `AETHER_BACKEND_ORION`: a backend declaring the `orion` namespace gets `audio.pc` / `audio.pc.state` | PC Audio decides whether slice volume and mute drive the RSP2 audio or the Orion's own AF (`*UM`/`*US`); upstream passes it to the `icom` namespace only |
| `src/gui/ConnectionPanel.h` | `kFamilyOrion` constant; `connectSerialRadio()` / `isSerialRadioSerial()` | Same pattern as the other families; startup auto-connect for serial radios |
| `src/gui/ConnectionPanel.cpp` | Family normalisation (two places), the "Ten-Tec Orion (serial)" radio-type entry, the hint and placeholder text, and an `orion` branch at the top of `probeRadio()` that hands the device path to the session as `orion:<path>` and remembers it (`SerialDevice/<family>`); the serial-family table (`kSerialRadioFamilies`, Omni VII joins it at step 5) that fills the device box; `connectSerialRadio()` | The manual-connect page is how the operator picks the radio; a serial radio has nothing to probe |
| `src/gui/MainWindow.cpp` | In the startup auto-connect block, under `AETHER_BACKEND_ORION`: a saved `<family>:/dev/...` serial connects directly via `connectSerialRadio()` | "Connect to last radio on start" waits for discovery or a routed IP, which a serial radio never gives |
| `src/gui/PhoneCwApplet.cpp` | `setSelectableMicInputs()` uses the model's mic-input list when it has one, instead of always FlexRadio's MIC/BAL/LINE/ACC/PC | Shows the Orion's MIC and PC; a Flex is unchanged (its list comes from `mic list`) |
| `tests/tests.cmake` | `orion_cat_test`, `orion_spectrum_test`, `orion_rxaudio_test`, `orion_peripherals_test`, `orion_skimmer_test` and `orion_backend_test` appended at the end | Upstream requires every test target to be declared there |
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
  `FldigiCwEngine`, `BayesCwEngine`, `SkimServer`: ported from the Orion
  console, GPL-2.0-or-later, namespace and include paths only, plus the
  engine watching the LO instead of the dial; built with OpenMP when found)
- `src/gui/orion/OrionSetupPage.cpp/.h` (Radio Setup -> Orion; built
  into the AetherSDR target from `cmake/AetherOrion.cmake`)
- `tests/orion_cat_test.cpp`, `tests/orion_backend_test.cpp`,
  `tests/orion_spectrum_test.cpp`, `tests/orion_rxaudio_test.cpp`,
  `tests/orion_peripherals_test.cpp`, `tests/orion_skimmer_test.cpp`
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
