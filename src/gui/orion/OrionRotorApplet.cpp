#include "gui/orion/OrionRotorApplet.h"

#include <QAction>
#include <QDialog>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMenu>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

#include "core/backends/IRadioBackend.h"
#include "gui/GreenHeronApplet.h"   // RotorCompass
#include "models/RadioModel.h"

namespace AetherSDR {

namespace {

quint64 nextRequestId()
{
    static quint64 id = 0x524f5400ull;
    return ++id;
}

bool orionConnected(RadioModel* model)
{
    return model && model->backend() && model->isConnected()
        && model->backendDeclaresExtension(QStringLiteral("orion"));
}

}  // namespace

OrionRotorApplet::OrionRotorApplet(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(6);

    m_compass = new RotorCompass(this);
    m_compass->setVisible(false);   // floating only, as the GHE tile does it
    v->addWidget(m_compass, 0, Qt::AlignHCenter);

    // What the rotor SAYS, never what we asked for.
    m_readout = new QLabel(this);
    m_readout->setObjectName(QStringLiteral("orionRotorReadout"));
    m_readout->setAlignment(Qt::AlignHCenter);
    v->addWidget(m_readout);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    m_bearing = new QSpinBox(this);
    m_bearing->setObjectName(QStringLiteral("orionRotorBearing"));
    m_bearing->setRange(0, 360);
    m_bearing->setSuffix(QStringLiteral("°"));
    m_bearing->setWrapping(true);
    m_bearing->setToolTip(tr("The bearing to turn to. Nothing moves until you press Turn\n"
                             "or Enter — picking a number never aims the antenna."));
    row->addWidget(m_bearing, 1);
    m_turn = new QPushButton(tr("Turn"), this);
    m_turn->setObjectName(QStringLiteral("orionRotorTurn"));
    m_turn->setToolTip(tr("Turn the rotor to the bearing on the left."));
    row->addWidget(m_turn);
    m_stop = new QPushButton(tr("Stop"), this);
    m_stop->setObjectName(QStringLiteral("orionRotorStop"));
    m_stop->setToolTip(tr("Stop the rotor where it is."));
    row->addWidget(m_stop);
    v->addLayout(row);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("orionRotorStatus"));
    m_status->setWordWrap(true);
    v->addWidget(m_status);
    v->addStretch(1);

    connect(m_turn, &QPushButton::clicked, this,
            [this] { turnTo(m_bearing->value()); });
    connect(m_bearing, &QSpinBox::editingFinished, this, [] {});   // no send on edit
    connect(m_stop, &QPushButton::clicked, this, &OrionRotorApplet::stopRotor);

    applyState({});
}

void OrionRotorApplet::setModel(RadioModel* model)
{
    if (m_model == model) {
        return;
    }
    m_model = model;
    if (!m_model) {
        applyState({});
        return;
    }
    // Live azimuth: the backend posts it as it changes, re-armed on each
    // connect so a reconnect does not leave a dead listener behind.
    auto guard = std::make_shared<QPointer<QObject>>();
    connect(m_model, &RadioModel::connectionStateChanged, this,
            [this, guard](bool connected) {
        delete guard->data();
        *guard = nullptr;
        if (!connected || !orionConnected(m_model)) {
            m_reported = -1.0;
            m_hasAsked = false;
            applyState({});
            return;
        }
        *guard = new QObject(this);
        connect(m_model->backend(), &IRadioBackend::extensionStatus, guard->data(),
                [this](const QString& ns, const QString& kind, const QVariantMap& f) {
            if (ns != QLatin1String("orion") || kind != QLatin1String("rotor")) {
                return;
            }
            m_reported = f.value(QStringLiteral("azimuth"), -1.0).toDouble();
            applyState({});
        });
        refresh();
    });
    refresh();
}

void OrionRotorApplet::setFloating(bool on)
{
    m_compass->setVisible(on);
}

void OrionRotorApplet::showBearing(double degrees)
{
    if (degrees >= 0.0 && degrees <= 360.0) {
        m_bearing->setValue(static_cast<int>(std::lround(degrees)));
    }
}

void OrionRotorApplet::turnTo(double degrees)
{
    if (!orionConnected(m_model)) {
        note(tr("No Orion connected."));
        return;
    }
    if (!(degrees >= 0.0 && degrees <= 360.0)) {
        return;
    }
    m_asked = degrees;
    m_hasAsked = true;
    QVariantMap arg;
    arg.insert(QStringLiteral("azimuth"), degrees);
    const quint64 id = nextRequestId();
    auto* g = new QObject(this);
    connect(m_model->backend(), &IRadioBackend::extensionResult, g,
            [this, g, id](quint64 got, const QVariant& result) {
        if (got != id) return;
        g->deleteLater();
        applyState(result.toMap());
    });
    m_model->invokeBackendExtension(QStringLiteral("orion"), QStringLiteral("rotor.turn"), id,
                                    arg);
    applyState({});
}

void OrionRotorApplet::stopRotor()
{
    if (!orionConnected(m_model)) {
        note(tr("No Orion connected."));
        return;
    }
    const quint64 id = nextRequestId();
    auto* g = new QObject(this);
    connect(m_model->backend(), &IRadioBackend::extensionResult, g,
            [this, g, id](quint64 got, const QVariant& result) {
        if (got != id) return;
        g->deleteLater();
        applyState(result.toMap());
    });
    m_model->invokeBackendExtension(QStringLiteral("orion"), QStringLiteral("rotor.stop"), id);
    m_hasAsked = false;   // nothing is being aimed at any more
    applyState({});
}

void OrionRotorApplet::refresh()
{
    if (!orionConnected(m_model)) {
        applyState({});
        return;
    }
    const quint64 id = nextRequestId();
    auto* g = new QObject(this);
    connect(m_model->backend(), &IRadioBackend::extensionResult, g,
            [this, g, id](quint64 got, const QVariant& result) {
        if (got != id) return;
        g->deleteLater();
        const QVariantMap m = result.toMap();
        m_reported = m.value(QStringLiteral("azimuth"), -1.0).toDouble();
        applyState(m);
    });
    m_model->invokeBackendExtension(QStringLiteral("orion"), QStringLiteral("rotor.get"), id);
}

// The readout is the REPORTED heading, with the asked-for one beside it and
// the difference — no "on target" verdict: a rotor stops short, overshoots
// and wanders at rest, so any threshold would only flicker.
void OrionRotorApplet::applyState(const QVariantMap& state)
{
    if (state.contains(QStringLiteral("azimuth"))) {
        m_reported = state.value(QStringLiteral("azimuth"), -1.0).toDouble();
    }
    const bool connected = orionConnected(m_model);
    const bool open = connected && state.value(QStringLiteral("open"), m_reported >= 0.0).toBool();
    m_bearing->setEnabled(connected);
    m_turn->setEnabled(connected);
    m_stop->setEnabled(connected);

    QString text;
    if (m_reported >= 0.0) {
        text = QStringLiteral("%1°").arg(m_reported, 0, 'f', 1);
        if (m_hasAsked) {
            double delta = std::fabs(m_reported - m_asked);
            if (delta > 180.0) {
                delta = 360.0 - delta;
            }
            text += tr(" · asked %1° · Δ%2°")
                        .arg(m_asked, 0, 'f', 1)
                        .arg(delta, 0, 'f', 1);
        }
    } else {
        text = tr("—°");
    }
    m_readout->setText(text);
    m_compass->setHeading(m_reported >= 0.0 ? m_reported : 0.0, m_hasAsked, m_asked);

    const QString err = state.value(QStringLiteral("error")).toString();
    if (!err.isEmpty()) {
        note(err);
    } else if (!connected) {
        note(tr("No Orion connected."));
    } else if (!open) {
        note(state.value(QStringLiteral("status")).toString());
    } else {
        note(QString());
    }
}

void OrionRotorApplet::note(const QString& text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void installOrionRotorWindow(QMainWindow* window, QMenu* menu, RadioModel* model)
{
    auto* act = menu->addAction(QObject::tr("Rotor..."));
    act->setToolTip(QObject::tr("The DCU-3: where the beam is pointing, and the one place a "
                                "turn is sent from"));
    auto held = std::make_shared<QPointer<QDialog>>();
    QObject::connect(act, &QAction::triggered, window, [=] {
        if (!*held) {
            auto* dlg = new QDialog(window);
            dlg->setWindowTitle(QObject::tr("Rotor"));
            dlg->setObjectName(QStringLiteral("orionRotorWindow"));
            auto* lay = new QVBoxLayout(dlg);
            lay->setContentsMargins(0, 0, 0, 0);
            auto* applet = new OrionRotorApplet(dlg);
            applet->setModel(model);
            applet->setFloating(true);   // a window: the compass earns its space
            lay->addWidget(applet);
            dlg->resize(280, 420);
            *held = dlg;
        }
        (*held)->show();
        (*held)->raise();
        (*held)->activateWindow();
    });
}

}  // namespace AetherSDR
