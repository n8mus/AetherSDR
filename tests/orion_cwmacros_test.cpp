// N8EM fork: the CW window's plain-language macro expansion.
//
// The vocabulary is the Orion console's, ported: a macro is a sentence
// with named holes ("[his call] DE [my call]"), the old cqrlog-style
// %mc / %c spellings still key, an unknown field stays visible instead
// of silently keying nothing, and an empty field is reported so the
// window can refuse to put a hole on the air.

#include "core/backends/orion/CwMacros.h"

#include <cstdio>

using AetherSDR::orion::CwMacroFields;
using AetherSDR::orion::cwGreetingFor;
using AetherSDR::orion::cwMacroFields;
using AetherSDR::orion::expandCwMacro;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void checkEq(const QString& got, const QString& want, const char* what)
{
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", what,
                     got.toUtf8().constData(), want.toUtf8().constData());
        ++g_failures;
    }
}

static CwMacroFields station()
{
    CwMacroFields f;
    f.myCall = QStringLiteral("N8EM");
    f.myName = QStringLiteral("JON");
    f.myQth = QStringLiteral("TOLEDO");
    f.myGrid = QStringLiteral("EN81");
    f.hisCall = QStringLiteral("W1AW");
    f.hisHandle = QStringLiteral("BOB");
    f.hisName = QStringLiteral("ROBERT");
    f.hisQth = QStringLiteral("NEWINGTON");
    f.hisGrid = QStringLiteral("FN31");
    f.rstSent = QStringLiteral("599");
    f.rstRcvd = QStringLiteral("579");
    f.greeting = QStringLiteral("GM");
    return f;
}

int main()
{
    const CwMacroFields f = station();

    checkEq(expandCwMacro(QStringLiteral("[his call] DE [my call] UR RST [rst sent] BK"), f),
            QStringLiteral("W1AW DE N8EM UR RST 599 BK"), "a plain-language macro expands");

    // The handle is what he SAID, kept apart from the callbook's name.
    checkEq(expandCwMacro(QStringLiteral("TU [his handle] 73"), f),
            QStringLiteral("TU BOB 73"), "[his handle] sends the on-air name");
    checkEq(expandCwMacro(QStringLiteral("TU [his name] 73"), f),
            QStringLiteral("TU ROBERT 73"), "[his name] sends the callbook name");

    // Case and inner spacing don't matter.
    checkEq(expandCwMacro(QStringLiteral("[ HIS  CALL ] DE [My Call]"), f),
            QStringLiteral("W1AW DE N8EM"), "field names are case- and space-insensitive");

    // The pre-2026 spellings still key exactly as they did.
    checkEq(expandCwMacro(QStringLiteral("%c DE %mc %mc UR RST %r"), f),
            QStringLiteral("W1AW DE N8EM N8EM UR RST 599"), "the old %c / %mc spellings still key");

    // A typo stays visible rather than keying a hole.
    checkEq(expandCwMacro(QStringLiteral("[his calls] DE [my call]"), f),
            QStringLiteral("[his calls] DE N8EM"), "an unknown field is left in the text");

    // An empty field is named, and the doubled space it leaves is collapsed.
    {
        CwMacroFields empty = f;
        empty.hisCall.clear();
        QString missing;
        const QString out =
            expandCwMacro(QStringLiteral("[his call] DE [my call]"), empty, &missing);
        checkEq(missing, QStringLiteral("his call"), "an empty field is reported by name");
        checkEq(out, QStringLiteral("DE N8EM"), "the gap an empty field leaves is collapsed");
    }

    // The greeting follows HIS clock, not ours.
    {
        const QString west = cwGreetingFor(QStringLiteral("DM12"));   // US west coast
        const QString east = cwGreetingFor(QStringLiteral("JO01"));   // western Europe
        check(west == QLatin1String("GM") || west == QLatin1String("GA")
                  || west == QLatin1String("GE"),
              "a greeting is one of GM / GA / GE");
        check(!east.isEmpty(), "a greeting is produced for an eastern grid");
        check(cwGreetingFor(QString()) == QLatin1String("GM")
                  || cwGreetingFor(QString()) == QLatin1String("GA")
                  || cwGreetingFor(QString()) == QLatin1String("GE"),
              "an empty grid falls back to our own clock");
    }

    // The editor's chip list names every field the expander knows.
    {
        bool sawHandle = false;
        for (const auto& fi : cwMacroFields()) {
            QString missing;
            const QString out = expandCwMacro(QLatin1String(fi.token), f, &missing);
            check(out != QLatin1String(fi.token),
                  "every chip in the editor is a field the expander knows");
            if (QLatin1String(fi.token) == QLatin1String("[his handle]")) {
                sawHandle = true;
            }
        }
        check(sawHandle, "the handle is offered in the macro editor");
    }

    if (g_failures == 0) {
        std::printf("orion_cwmacros_test: all checks passed\n");
        return 0;
    }
    std::printf("orion_cwmacros_test: %d failure(s)\n", g_failures);
    return 1;
}
