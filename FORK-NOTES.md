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
| `src/gui/ConnectionPanel.h` | `kFamilyOrion` constant | Same pattern as the other families |
| `src/gui/ConnectionPanel.cpp` | Family normalisation (two places), the "Ten-Tec Orion (serial)" radio-type entry, the hint and placeholder text, and an `orion` branch at the top of `probeRadio()` that hands the device path to the session as `orion:<path>` | The manual-connect page is how the operator picks the radio; a serial radio has nothing to probe |
| `tests/tests.cmake` | `orion_cat_test`, `orion_spectrum_test`, `orion_rxaudio_test` and `orion_backend_test` appended at the end | Upstream requires every test target to be declared there |
| `tests/backend_seam_affinity_test.cpp` | `orion` added to the constructed-families list | The seam contract test covers the new backend too |

New files that upstream does not have (no merge risk):

- `FORK-NOTES.md` (this file)
- `cmake/AetherOrion.cmake`
- `docs/architecture/orion-backend-design.md`
- `src/core/backends/orion/` (`OrionBackend`, `OrionCat`, `OrionPollScheduler`,
  `OrionTxAudio`, and for the RSP2 `Rsp2Receiver`, `OrionSpectrum`,
  `OrionRxAudio`, `SdrPlayApi`)
- `tests/orion_cat_test.cpp`, `tests/orion_backend_test.cpp`,
  `tests/orion_spectrum_test.cpp`, `tests/orion_rxaudio_test.cpp`
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
