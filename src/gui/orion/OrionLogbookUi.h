#pragma once

// N8EM fork: the station logbook's screens (step 9, stage 1), in AetherSDR's
// own widgets: a "Logbook" menu (Log QSO..., ADIF import/export, the cqrlog
// switches) and the Log QSO window. The log itself is the Orion console's,
// ported (core/backends/orion/log/). See FORK-NOTES.md.

#include <functional>

#include <QColor>
#include <QString>

class QMainWindow;
class QMenuBar;

namespace AetherSDR {

namespace orion::logbook {
class QsoContext;
}

class RadioModel;
class SliceModel;

// Creates the logbook (opening its log and starting the cqrlog pull) and
// adds the Logbook menu to `bar`. `activeSlice` supplies the frequency and
// mode the QSO window fills in.
void installOrionLogbook(QMainWindow* window, QMenuBar* bar, RadioModel* model,
                         std::function<SliceModel*()> activeSlice);

// The station being worked right now, shared by the Log QSO window and the
// CW window (core/backends/orion/log/QsoContext.h). Null until
// installOrionLogbook() has run.
orion::logbook::QsoContext* orionQsoContext();

// Worked-before colour for a spot, as the console colours them: never
// worked red, worked on another band amber, worked this band green,
// confirmed grey. Invalid when the logbook has no data, the call is
// unknown, or the operator turned the colours off.
QColor orionWorkedSpotColour(const QString& call, double freqMhz);

}  // namespace AetherSDR
