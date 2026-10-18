#pragma once

// N8EM fork: the station logbook's screens (step 9, stage 1), in AetherSDR's
// own widgets: a "Logbook" menu (Log QSO..., ADIF import/export, the cqrlog
// switches) and the Log QSO window. The log itself is the Orion console's,
// ported (core/backends/orion/log/). See FORK-NOTES.md.

#include <functional>

class QMainWindow;
class QMenuBar;

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Creates the logbook (opening its log and starting the cqrlog pull) and
// adds the Logbook menu to `bar`. `activeSlice` supplies the frequency and
// mode the QSO window fills in.
void installOrionLogbook(QMainWindow* window, QMenuBar* bar, RadioModel* model,
                         std::function<SliceModel*()> activeSlice);

}  // namespace AetherSDR
