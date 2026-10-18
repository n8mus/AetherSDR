#pragma once

// N8EM fork: the Radio Setup page for the Ten-Tec Orion's own audio routing
// (*UC): which receiver each headphone side and the speaker hear. Built from
// AetherSDR's own widgets; it talks to the backend only through the "orion"
// extension (audio.routing.get / audio.routing.set). See FORK-NOTES.md.

class QWidget;

namespace AetherSDR {

class RadioModel;

QWidget* buildOrionAudioPage(RadioModel* model);

}  // namespace AetherSDR
