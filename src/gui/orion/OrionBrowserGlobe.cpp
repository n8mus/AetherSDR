// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/util/BrowserGlobe.cpp) for the
// AetherSDR Orion fork: namespace, include paths and resource prefix only.
// The page itself is the operator's cqrlog fork's globe template, verbatim.
#include "gui/orion/OrionBrowserGlobe.h"

#include <QDesktopServices>
#include <QDir>
#include <cmath>
#include <QFile>
#include <QStandardPaths>
#include <QUrl>

namespace AetherSDR::orion {
namespace BrowserGlobe {

namespace {
// The page is the cqrlog fork's cGlobeTemplate, verbatim (same look, same
// buttons, same country handling) — only the title suffix differs.
const char* kTemplate = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>@@DXCALL@@ - globe</title>
<style>
  html,body { margin:0; padding:0; height:100%; background:#000; overflow:hidden;
              font-family:"DejaVu Sans",sans-serif; color:#e8e8e8; }
  #globe { position:absolute; inset:0; }
  #info { position:absolute; top:12px; left:12px; z-index:10; padding:10px 14px;
          background:rgba(10,14,22,.78); border:1px solid #2c3a4d; border-radius:6px;
          font-size:13px; line-height:1.45; max-width:19em; }
  #info h1 { margin:0 0 4px; font-size:19px; letter-spacing:.5px; color:#ff8a6a; }
  #info .me { color:#7cf07c; }
  #info .k { color:#8fa6bf; display:inline-block; min-width:5.2em; }
  #info .note { color:#c9a227; font-size:11px; margin-top:6px; }
  #btns { position:absolute; bottom:12px; left:12px; z-index:10; }
  #btns button { background:rgba(10,14,22,.78); color:#dfe6ee; border:1px solid #2c3a4d;
                 border-radius:5px; padding:5px 11px; margin-right:6px; font-size:12px;
                 cursor:pointer; }
  #btns button:hover { background:#1b2636; }
  #btns button.off { color:#68727f; }
  #hint { position:absolute; bottom:14px; right:14px; z-index:10; font-size:11px; color:#7e8896; }
</style>
</head>
<body>
<div id="globe"></div>
<div id="info">
  <h1>@@DXCALL@@</h1>
  <div><span class="k">Grid</span>@@DXLOC@@</div>
  <div><span class="k">Lat/Lon</span>@@DXLATTXT@@ @@DXLONTXT@@</div>
  <div><span class="k">Distance</span>@@DIST@@</div>
  <div><span class="k">Beam</span>@@AZIM@@&deg; SP / @@AZIMLP@@&deg; LP</div>
  <div style="margin-top:6px"><span class="k">Home</span><span class="me">@@MYCALL@@</span> @@MYLOC@@</div>
  <div class="note">@@NOTE@@</div>
</div>
<div id="btns">
  <button onclick="spin()">Spin</button>
  <button onclick="aimPath()">Path</button>
  <button onclick="aimDx()">DX</button>
  <button onclick="aimMe()">Home</button>
  <button id="bBorders" onclick="toggleBorders(this)">Borders</button>
  <button id="bNames" onclick="toggleNames(this)">Names</button>
</div>
<div id="hint">drag to rotate &middot; scroll to zoom</div>
<script src="@@GLOBEJS@@"></script>
<script>
var MY = { lat: @@MYLAT@@, lng: @@MYLON@@, call: "@@MYCALL@@" };
var DX = { lat: @@DXLAT@@, lng: @@DXLON@@, call: "@@DXCALL@@" };
var WORLD = @@COUNTRIES@@;

var stations = [
  { lat: MY.lat, lng: MY.lng, size: 0.9, color: "#4cff4c", label: MY.call,
    txt: 1.7, dot: 0.35, alt: 0.016 },
  { lat: DX.lat, lng: DX.lng, size: 1.1, color: "#ff5a3c", label: DX.call,
    txt: 1.7, dot: 0.35, alt: 0.016 }
];

var showBorders = true, showNames = true, minName = 0;

function countryLabels() {
  if (!showNames) return [];
  return WORLD.labels.filter(function (c) { return c.s >= minName; }).map(function (c) {
    return { lat: c.lat, lng: c.lng, label: c.n, color: "rgba(226,238,252,.72)",
             txt: c.s * 0.95, dot: 0, alt: 0.006 };
  });
}
function paintLabels() { world.labelsData(stations.concat(countryLabels())); }

var world = Globe()
  (document.getElementById("globe"))
  .globeImageUrl("@@TEXTURE@@")
  .backgroundColor("#000008")
  .showAtmosphere(true)
  .atmosphereColor("#6fb3ff")
  .pointsData(stations)
  .pointLat("lat").pointLng("lng").pointColor("color")
  .pointAltitude(0.012).pointRadius("size")
  .labelLat("lat").labelLng("lng").labelText("label").labelColor("color")
  .labelSize("txt").labelDotRadius("dot").labelAltitude("alt").labelResolution(2)
  .polygonsData(WORLD.features)
  .polygonCapColor(function () { return "rgba(0,0,0,0)"; })
  .polygonSideColor(function () { return "rgba(0,0,0,0)"; })
  .polygonStrokeColor(function () { return "rgba(226,238,252,.55)"; })
  .polygonAltitude(0.004)
  .polygonLabel(function (d) { return d.properties.n; })
  .arcsData([{ sLat: MY.lat, sLng: MY.lng, eLat: DX.lat, eLng: DX.lng }])
  .arcStartLat("sLat").arcStartLng("sLng").arcEndLat("eLat").arcEndLng("eLng")
  .arcColor(function () { return ["#4cff4c", "#ffd24c", "#ff5a3c"]; })
  .arcStroke(0.55)
  .arcAltitudeAutoScale(0.45);

world.controls().autoRotateSpeed = 0.35;

function toggleBorders(b) {
  showBorders = !showBorders;
  b.className = showBorders ? "" : "off";
  world.polygonsData(showBorders ? WORLD.features : []);
}
function toggleNames(b) {
  showNames = !showNames;
  b.className = showNames ? "" : "off";
  paintLabels();
}

function rad(d) { return d * Math.PI / 180; }
function midPoint(a, b) {
  var f1 = rad(a.lat), l1 = rad(a.lng), f2 = rad(b.lat), l2 = rad(b.lng);
  var bx = Math.cos(f2) * Math.cos(l2 - l1), by = Math.cos(f2) * Math.sin(l2 - l1);
  var lat = Math.atan2(Math.sin(f1) + Math.sin(f2),
                       Math.sqrt(Math.pow(Math.cos(f1) + bx, 2) + by * by));
  var lng = l1 + Math.atan2(by, Math.cos(f1) + bx);
  return { lat: lat * 180 / Math.PI, lng: ((lng * 180 / Math.PI) + 540) % 360 - 180 };
}
function angDist(a, b) {
  var f1 = rad(a.lat), f2 = rad(b.lat), df = rad(b.lat - a.lat), dl = rad(b.lng - a.lng);
  var h = Math.sin(df / 2) * Math.sin(df / 2) +
          Math.cos(f1) * Math.cos(f2) * Math.sin(dl / 2) * Math.sin(dl / 2);
  return 2 * Math.atan2(Math.sqrt(h), Math.sqrt(1 - h)) * 180 / Math.PI;
}

var mid = midPoint(MY, DX);
var span = angDist(MY, DX);
var alt = Math.max(1.6, Math.min(4.2, 0.9 + span / 60));

function aimPath() { world.pointOfView({ lat: mid.lat, lng: mid.lng, altitude: alt }, 900); }
function aimDx()   { world.pointOfView({ lat: DX.lat, lng: DX.lng, altitude: 1.1 }, 900); }
function aimMe()   { world.pointOfView({ lat: MY.lat, lng: MY.lng, altitude: 1.1 }, 900); }
function spin()    { var c = world.controls(); c.autoRotate = !c.autoRotate; }

// thin the country names out when zoomed away, the way a paper map does
function nameFloor(a) { return a > 2.2 ? 0.85 : (a > 1.3 ? 0.6 : (a > 0.7 ? 0.42 : 0)); }
minName = nameFloor(alt);
world.onZoom(function (pov) {
  var f = nameFloor(pov.altitude);
  if (f !== minName) { minName = f; paintLabels(); }
});

paintLabels();
aimPath();
window.addEventListener("resize", function () {
  world.width(window.innerWidth); world.height(window.innerHeight);
});
</script>
</body>
</html>)HTML";

QString safeText(QString s) {
    s.remove('"');
    s.remove('\\');
    s.remove('<');
    s.remove('>');
    return s.trimmed();
}

QString jsNum(double v) {
    return QString::number(v, 'f', 4);
}

QString coordText(double v, const char* pos, const char* neg) {
    return QString::number(std::abs(v), 'f', 2)
        + QLatin1String(v < 0 ? neg : pos);
}

// Stage a resource next to the page (a file:// page may only load what
// sits beside it).
bool stage(const QString& res, const QString& dst) {
    if (QFile::exists(dst)) return true;
    return QFile::copy(res, dst);
}
} // namespace

void show(double myLat, double myLon, const QString& myCall,
          const QString& myLoc, double dxLat, double dxLon,
          const QString& dxCall, const QString& dxLoc,
          const QString& distText, int azimDeg, const QString& note) {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/globe";
    QDir().mkpath(dir);
    stage(":/orion-globe/globe.gl.min.js", dir + "/globe.gl.min.js");

    // Texture as a data: URI, countries inlined — the page needs no net.
    QString texRef =
        "https://unpkg.com/three-globe/example/img/earth-blue-marble.jpg";
    QFile tex(":/orion-globe/earth-blue-marble.jpg");
    if (tex.open(QIODevice::ReadOnly))
        texRef = "data:image/jpeg;base64,"
            + QString::fromLatin1(tex.readAll().toBase64());
    QString world = "{\"features\":[],\"labels\":[]}";
    QFile cj(":/orion-globe/countries.json");
    if (cj.open(QIODevice::ReadOnly)) world = QString::fromUtf8(cj.readAll());

    QString html = QLatin1String(kTemplate);
    html.replace("@@COUNTRIES@@", world);
    html.replace("@@GLOBEJS@@", "globe.gl.min.js");
    html.replace("@@TEXTURE@@", texRef);
    html.replace("@@MYLAT@@", jsNum(myLat));
    html.replace("@@MYLON@@", jsNum(myLon));
    html.replace("@@DXLAT@@", jsNum(dxLat));
    html.replace("@@DXLON@@", jsNum(dxLon));
    html.replace("@@MYCALL@@", safeText(myCall));
    html.replace("@@MYLOC@@", safeText(myLoc));
    const QString dc = safeText(dxCall);
    html.replace("@@DXCALL@@",
                 dc.isEmpty() ? QStringLiteral("DX station") : dc);
    html.replace("@@DXLOC@@", safeText(dxLoc));
    html.replace("@@DXLATTXT@@", coordText(dxLat, "N", "S"));
    html.replace("@@DXLONTXT@@", coordText(dxLon, "E", "W"));
    html.replace("@@DIST@@", safeText(distText));
    html.replace("@@AZIM@@", QString::number(azimDeg));
    html.replace("@@AZIMLP@@", QString::number((azimDeg + 180) % 360));
    html.replace("@@NOTE@@", safeText(note));

    const QString page = dir + "/globe.html";
    QFile f(page);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(html.toUtf8());
    f.close();
    QDesktopServices::openUrl(QUrl::fromLocalFile(page));
}

} // namespace BrowserGlobe


}  // namespace AetherSDR::orion
