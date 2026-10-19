// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/cw/CwMacros.cpp) for the AetherSDR
// Orion fork (CW window, step 10): namespace and include paths only.
#include "core/backends/orion/CwMacros.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QTimeZone>

namespace AetherSDR::orion {

namespace {

// Normalized key ("his call") -> the field's value in f.
QString valueFor(const QString& key, const CwMacroFields& f, bool* known) {
    static const QHash<QString, int> kIndex = {
        {"my call", 0},   {"my name", 1},   {"my qth", 2},  {"my grid", 3},
        {"his call", 4},  {"his handle", 5}, {"his name", 6},
        {"his qth", 7},   {"his grid", 8},
        {"rst sent", 9},  {"rst rcvd", 10}, {"greeting", 11},
    };
    const auto it = kIndex.constFind(key);
    *known = it != kIndex.constEnd();
    if (!*known) return QString();
    switch (*it) {
        case 0: return f.myCall;
        case 1: return f.myName;
        case 2: return f.myQth;
        case 3: return f.myGrid;
        case 4: return f.hisCall;
        case 5: return f.hisHandle;
        case 6: return f.hisName;
        case 7: return f.hisQth;
        case 8: return f.hisGrid;
        case 9: return f.rstSent;
        case 10: return f.rstRcvd;
        default: return f.greeting;
    }
}

// The pre-2026-10-19 spellings, rewritten to bracket form before the main
// pass so an old macro keys unchanged. Longest first: %mc must not be
// eaten by %m, and %rs isn't ours (cqrlog's cut-number form) so it is
// left alone rather than half-matched by %r.
void rewriteLegacy(QString& t) {
    static const QList<QPair<QString, QString>> kOld = {
        {"%mc", "[my call]"},  {"%mn", "[my name]"}, {"%mq", "[my qth]"},
        {"%ml", "[my grid]"},  {"%c", "[his call]"}, {"%n", "[his name]"},
        {"%h", "[greeting]"},  {"%r", "[rst sent]"},
    };
    for (const auto& p : kOld)
        t.replace(p.first, p.second, Qt::CaseInsensitive);
}

}  // namespace

const QList<CwFieldInfo>& cwMacroFields() {
    static const QList<CwFieldInfo> kFields = {
        {"[his call]", "the call in the DX box"},
        {"[his handle]", "the on-air name he gave you (the Log QSO window's HANDLE box)"},
        {"[his name]", "the callbook name (the Log QSO window's NAME box)"},
        {"[his qth]", "his town, from the callbook"},
        {"[his grid]", "his grid square"},
        {"[rst sent]", "the report you're giving him (the Log QSO window's RST SENT)"},
        {"[rst rcvd]", "the report he gave you (the Log QSO window's RST RCVD)"},
        {"[my call]", "your callsign (Settings -> Station)"},
        {"[my name]", "your name (Settings -> Station)"},
        {"[my qth]", "your town (Settings -> Station)"},
        {"[my grid]", "your grid square (Settings -> Station)"},
        {"[greeting]", "GM / GA / GE, by HIS local time"},
    };
    return kFields;
}

QString cwGreetingFor(const QString& grid) {
    QDateTime his = QDateTime::currentDateTime();
    // Maidenhead longitude -> his mean solar time. Two fields are plenty
    // (a 20-degree square is about an hour and a bit; the greeting only
    // has to land in the right third of the day).
    const QString g = grid.trimmed().toUpper();
    if (g.size() >= 4 && g[0] >= 'A' && g[0] <= 'R' && g[2].isDigit()) {
        const double lon = (g[0].unicode() - 'A') * 20.0
                           + (g[2].unicode() - '0') * 2.0 - 180.0 + 1.0;
        his = QDateTime::currentDateTimeUtc().addSecs(
            qint64(lon / 15.0 * 3600.0));
    }
    const int h = his.time().hour();
    if (h < 12) return QStringLiteral("GM");
    if (h < 18) return QStringLiteral("GA");
    return QStringLiteral("GE");
}

QString expandCwMacro(QString text, const CwMacroFields& f,
                      QString* missing) {
    if (missing) missing->clear();
    rewriteLegacy(text);
    static const QRegularExpression kHole(QStringLiteral("\\[([^\\]]*)\\]"));
    QString out;
    out.reserve(text.size());
    int at = 0;
    auto it = kHole.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(at, m.capturedStart() - at);
        at = m.capturedEnd();
        // "[ HIS  CALL ]" and "[his call]" are the same hole.
        const QString key = m.captured(1).simplified().toLower();
        bool known = false;
        const QString v = valueFor(key, f, &known);
        if (!known) {                  // a typo stays visible
            out += m.captured(0);
            continue;
        }
        if (v.isEmpty() && missing && missing->isEmpty()) *missing = key;
        out += v;
    }
    out += text.mid(at);
    // An empty field leaves a double space, which keys an audible extra
    // word gap (the contest deck's lesson).
    return out.simplified();
}

}  // namespace AetherSDR::orion
