#include "gui/orion/OrionCwUi.h"

#include <QAction>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>

#include <functional>

#include "core/AppSettings.h"
#include "gui/orion/OrionCwWindow.h"
#ifdef AETHER_ORION_LOGBOOK
#include "gui/orion/OrionLogbookUi.h"
#endif
#include "models/CwRxModel.h"
#include "models/RadioModel.h"

namespace AetherSDR {

namespace {
// The one CW window, and how to raise it from elsewhere in the main window
// (the CWX indicator opens this instead of AetherSDR's CWX panel).
std::function<void()> g_open;
}  // namespace

bool orionCwReplacesCwxPanel()
{
    return AppSettings::instance()
        .value(QStringLiteral("OrionCw/replaceCwx"), true)
        .toBool();
}

void installOrionCwWindow(QMainWindow* window, QMenu* menu, RadioModel* model,
                          CwRxModel* decoder)
{
    menu->addSeparator();
    auto* act = menu->addAction(QObject::tr("CW window..."));
    act->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    act->setToolTip(QObject::tr("Type-ahead CW, F1-F12 macros in plain language, and the CW "
                                "reader — sharing the station being worked with the Log QSO "
                                "window"));

    auto cw = std::make_shared<QPointer<OrionCwWindow>>();
    const auto open = [=] {
        if (!*cw) {
            orion::logbook::QsoContext* ctx = nullptr;
#ifdef AETHER_ORION_LOGBOOK
            ctx = orionQsoContext();
#endif
            auto* w = new OrionCwWindow(&model->cwxModel(), ctx, window);
            *cw = w;

            // The operator's own station, for the [my ...] macro fields. The
            // callsign is AetherSDR's own; it has no setting for the
            // operator's name, town or grid, so those three are the fork's
            // (Radio Setup -> Orion).
            auto& s = AppSettings::instance();
            w->setStationFields(s.value(QStringLiteral("StationCallsign")).toString(),
                                s.value(QStringLiteral("OrionCw/myName")).toString(),
                                s.value(QStringLiteral("OrionCw/myQth")).toString(),
                                s.value(QStringLiteral("OrionCw/myGrid")).toString());

            if (decoder) {
                // AetherSDR's CW reader feeds the console-style pane. Both
                // text signals: a backend without a calibrated cost emits
                // the unscored one.
                QObject::connect(decoder, &CwRxModel::textDecoded, w,
                                 [w](const QString& text, float) { w->appendRx(text); });
                QObject::connect(decoder, &CwRxModel::unscoredTextDecoded, w,
                                 &OrionCwWindow::appendRx);
                QObject::connect(decoder, &CwRxModel::statsUpdated, w,
                                 [w](float pitch, float speed) { w->setRxStats(pitch, speed); });
            }
        }
        (*cw)->show();
        (*cw)->raise();
        (*cw)->activateWindow();
    };
    QObject::connect(act, &QAction::triggered, window, open);
    g_open = open;
}

bool openOrionCwWindow()
{
    if (!g_open) {
        return false;
    }
    g_open();
    return true;
}

}  // namespace AetherSDR
