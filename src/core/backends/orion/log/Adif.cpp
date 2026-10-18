// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from the N8EM Orion console (src/log/Adif.cpp) for the AetherSDR Orion
// fork (logbook, step 9): namespace and include paths, AetherSDR settings in
// place of QSettings, and AetherSDR's cty.dat parser in place of the console's.
#include "core/backends/orion/log/Adif.h"

#include <QIODevice>

namespace AetherSDR::orion::logbook {
namespace Adif {

QList<AdifRecord> parse(QIODevice& in) {
    return parseBytes(in.readAll());
}

// Scan "<name:len[:type]>data" tags in byte space. Everything between tags
// is filler (newlines, prose) and is ignored; <EOH> discards what came
// before it (the header), <EOR> closes a record.
QList<AdifRecord> parseBytes(const QByteArray& bytes) {
    QList<AdifRecord> out;
    AdifRecord cur;
    int i = 0;
    const int n = bytes.size();
    while (i < n) {
        const int lt = bytes.indexOf('<', i);
        if (lt < 0) break;
        const int gt = bytes.indexOf('>', lt + 1);
        if (gt < 0) break;
        const QByteArray tag = bytes.mid(lt + 1, gt - lt - 1);
        const QList<QByteArray> parts = tag.split(':');
        const QByteArray name = parts[0].trimmed().toUpper();
        if (name == "EOR") {
            if (!cur.isEmpty()) out.append(cur);
            cur.clear();
            i = gt + 1;
            continue;
        }
        if (name == "EOH") {                     // header ends; drop its fields
            cur.clear();
            i = gt + 1;
            continue;
        }
        if (parts.size() < 2) { i = gt + 1; continue; }   // <APP>-style, no data
        bool lenOk = false;
        const int len = parts[1].trimmed().toInt(&lenOk);
        if (!lenOk || len < 0) { i = gt + 1; continue; }
        const QByteArray val = bytes.mid(gt + 1, len);
        if (!name.isEmpty() && !val.isEmpty())
            cur.insert(QString::fromLatin1(name),
                       QString::fromUtf8(val).trimmed());
        i = gt + 1 + len;
    }
    if (!cur.isEmpty()) out.append(cur);         // file without a final <EOR>
    return out;
}

namespace {
void putField(QString& s, const char* name, const QString& val) {
    if (val.isEmpty()) return;
    const QByteArray utf = val.toUtf8();
    s += QString("<%1:%2>%3 ").arg(QLatin1String(name))
                              .arg(utf.size()).arg(val);
}
} // namespace

QString writeRecord(const AdifRecord& rec) {
    // Stable field order: the identity fields first, then the rest sorted,
    // so exports diff cleanly between runs.
    static const char* first[] = {"CALL", "QSO_DATE", "TIME_ON", "BAND",
                                  "FREQ", "MODE"};
    QString s;
    QStringList keys = rec.keys();
    for (const char* f : first) {
        const QString k = QLatin1String(f);
        if (rec.contains(k)) {
            putField(s, f, rec.value(k));
            keys.removeAll(k);
        }
    }
    keys.sort();
    for (const QString& k : keys)
        putField(s, k.toUtf8().constData(), rec.value(k));
    s += "<EOR>\n";
    return s;
}

QString fileHeader() {
    return QStringLiteral(
        "AetherSDR ADIF export\n"
        "<ADIF_VER:5>3.1.4 <PROGRAMID:9>AetherSDR <EOH>\n");
}

} // namespace Adif
} // namespace AetherSDR::orion::logbook
