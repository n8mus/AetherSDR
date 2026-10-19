// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Ported from the N8EM Orion console (src/cw/CwMacros.h) for the AetherSDR
// Orion fork (CW window, step 10): namespace and include paths only. The
// expansion itself is protocol, not appearance.

#include <QList>
#include <QString>

namespace AetherSDR::orion {

// CW macro text in PLAIN LANGUAGE (the operator's call). A macro is a
// sentence with holes; something has to mark a hole, and cqrlog's "%xnrs"
// spelling is unreadable a year later. Ours names the field:
//
//   [his call] DE [my call] UR RST [rst sent] [rst sent] NAME [my name] BK
//
// Same mechanism, readable cold. Case and inner spacing don't matter
// ([HIS CALL] == [his call]); an unknown field is left in the text
// verbatim so a typo is visible instead of silently keying nothing. The
// old %mc / %c / %mn / %mq / %ml / %n / %r spellings still work — macros
// written before this change key exactly as they did.
struct CwMacroFields {
    QString myCall, myName, myQth, myGrid;
    QString hisCall, hisHandle, hisName, hisQth, hisGrid;
    QString rstSent, rstRcvd;
    QString greeting;   // GM / GA / GE, by his local time
};

// The field vocabulary, in chip order for the editor: the text to insert
// and what it does.
struct CwFieldInfo {
    const char* token;
    const char* what;
};
const QList<CwFieldInfo>& cwMacroFields();

// Expand `text`. When a field the text asks for is empty, `missing` (if
// given) receives its name and the result is still expanded — the caller
// decides whether an incomplete message may go on the air. Collapses the
// doubled spaces an empty field leaves behind.
QString expandCwMacro(QString text, const CwMacroFields& f, QString* missing = nullptr);

// GM / GA / GE for a station at `grid` (his local morning/afternoon/
// evening, cqrlog's %h idea). Empty or unparsable grid: our own clock.
QString cwGreetingFor(const QString& grid);

}  // namespace AetherSDR::orion
