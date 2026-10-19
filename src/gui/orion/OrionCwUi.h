#pragma once

// N8EM fork: the CW window's menu entry and wiring (step 10). The window
// itself is gui/orion/OrionCwWindow.h. See FORK-NOTES.md.

#include <functional>

class QMainWindow;
class QMenu;

namespace AetherSDR {

class CwRxModel;
class RadioModel;

// Adds "CW window..." to `menu` and wires it: text keying through
// RadioModel's CwxModel, decoded CW from `decoder`, the station fields from
// AetherSDR's own station settings, and the shared current-QSO record from
// the logbook (so the Log QSO window and this one are about one station).
void installOrionCwWindow(QMainWindow* window, QMenu* menu, RadioModel* model,
                          CwRxModel* decoder);

// Shows the CW window (creating it on first use). False when
// installOrionCwWindow() has not run, so the caller can fall back.
bool openOrionCwWindow();

// True while the operator wants the fork's CW window instead of AetherSDR's
// CWX panel (fork rule 2's CW exception; setting OrionCw/replaceCwx,
// default on). MainWindow asks before it builds the CWX panel.
bool orionCwReplacesCwxPanel();

}  // namespace AetherSDR
