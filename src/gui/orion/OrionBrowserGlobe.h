// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Ported from the N8EM Orion console (src/util/BrowserGlobe.h) for the
// AetherSDR Orion fork: namespace and include paths only.
#include <QString>

namespace AetherSDR::orion {

// The 3D globe from the operator's cqrlog fork (NewQSO's globe button),
// ported whole: globe.gl in the default browser — blue-marble WebGL earth
// with country borders and zoom-thinned country names (the operator's own
// countries.json), both stations marked, the great-circle path arced
// between them, and the Spin / Path / DX / Home / Borders / Names buttons.
// Assets ride in the exe as resources and are staged next to the generated
// page (a file:// page loads scripts beside itself; the texture goes in as
// a data: URI).
namespace BrowserGlobe {

void show(double myLat, double myLon, const QString& myCall,
          const QString& myLoc, double dxLat, double dxLon,
          const QString& dxCall, const QString& dxLoc,
          const QString& distText, int azimDeg, const QString& note);

} // namespace BrowserGlobe


}  // namespace AetherSDR::orion
