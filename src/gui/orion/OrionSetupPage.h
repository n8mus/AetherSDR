#pragma once

// N8EM fork: the Radio Setup page for the Ten-Tec Orion (Radio -> Orion).
//  - Audio routing: which receiver each headphone side and the speaker hear
//    (*UC), the radio's own setting.
//  - CW skimmer: on/off, pause outside CW, the Bayes decoder, channels and
//    the telnet port, with a live status line.
// Built from AetherSDR's own widgets; it talks to the backend only through
// the "orion" extension (audio.routing.get/.set, skimmer.get/.set).
// See FORK-NOTES.md.

class QWidget;

namespace AetherSDR {

class RadioModel;

QWidget* buildOrionSetupPage(RadioModel* model);

}  // namespace AetherSDR
