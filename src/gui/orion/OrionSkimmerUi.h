#pragma once

// N8EM fork: the CW skimmer's windows (step 8, stage 2), opened from the
// Tools menu:
//  - Skimmer band map: the skimmer's finds as a frequency-sorted list
//    (call, WPM, age), coloured by worked-before status; click to tune.
//  - Skimmer waterfall: the CW segment at keying resolution (~8 ms
//    columns, ~30 Hz bins), frequency up the side, time flowing left, with
//    the decoded calls and their live copy riding next to the traces;
//    click to tune, wheel to zoom, drag to pan.
// Both are the Orion console's windows re-expressed in AetherSDR's widgets
// and colours (its waterfall scheme, its theme). See FORK-NOTES.md.

#include <functional>

class QMainWindow;
class QMenu;

namespace AetherSDR {

class RadioModel;
class SliceModel;

void installOrionSkimmerWindows(QMainWindow* window, QMenu* menu, RadioModel* model,
                                std::function<SliceModel*()> activeSlice);

}  // namespace AetherSDR
