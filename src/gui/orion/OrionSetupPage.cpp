#include "gui/orion/OrionSetupPage.h"

#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>

#include <array>
#include <functional>
#include <memory>

namespace AetherSDR {

namespace {

// Request ids are not allocated centrally (RadioSetupDialog's note); this
// decade is the fork's own so it cannot collide with the dialog's counters.
constexpr quint64 kOrionRequestIdBase = 0x0700000000000000ull;

quint64 nextRequestId()
{
    static quint64 next = kOrionRequestIdBase;
    return next++;
}

bool orionConnected(RadioModel* model)
{
    return model && model->backend() && model->isConnected()
        && model->backendDeclaresExtension(QStringLiteral("orion"));
}

// Ask the backend `verb` and hand the reply map to `fill` (on the GUI
// thread). The reply may come synchronously, so the handler is armed first.
void askOrion(RadioModel* model, QObject* context, const QString& verb,
              std::function<void(const QVariantMap&)> fill)
{
    IRadioBackend* backend = model->backend();
    const quint64 requestId = nextRequestId();
    auto* guard = new QObject(context);
    QObject::connect(backend, &IRadioBackend::extensionResult, guard,
                     [guard, requestId, fill](quint64 id, const QVariant& result) {
        if (id != requestId) return;
        guard->deleteLater();
        fill(result.toMap());
    });
    QObject::connect(backend, &IRadioBackend::extensionError, guard,
                     [guard, requestId](quint64 id, const QString&) {
        if (id == requestId) guard->deleteLater();
    });
    model->invokeBackendExtension(QStringLiteral("orion"), verb, requestId);
}

QComboBox* routingCombo()
{
    auto* combo = new QComboBox;
    combo->addItem(QObject::tr("VFO A (main)"), QStringLiteral("M"));
    combo->addItem(QObject::tr("VFO B (sub)"), QStringLiteral("S"));
    combo->addItem(QObject::tr("Both"), QStringLiteral("B"));
    return combo;
}

}  // namespace

QWidget* buildOrionSetupPage(RadioModel* model)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    auto* notConnected = new QLabel(
        QObject::tr("Connect the Orion to change these settings."));
    notConnected->setWordWrap(true);
    vbox->addWidget(notConnected);

    // ---- audio routing (*UC) -------------------------------------------------
    auto* audioBox = new QGroupBox(QObject::tr("Audio routing"));
    auto* audioLay = new QVBoxLayout(audioBox);
    auto* audioIntro = new QLabel(QObject::tr(
        "Which receiver you hear on the Orion's own outputs: each headphone side and "
        "the speaker can carry VFO A, VFO B or both. This is the radio's setting; it "
        "stays in the radio when AetherSDR closes."));
    audioIntro->setWordWrap(true);
    audioLay->addWidget(audioIntro);
    auto* audioForm = new QFormLayout;
    const std::array<const char*, 3> keys{"left", "right", "speaker"};
    const std::array<QString, 3> labels{QObject::tr("Headphones left:"),
                                        QObject::tr("Headphones right:"),
                                        QObject::tr("Speaker:")};
    auto combos = std::make_shared<std::array<QPointer<QComboBox>, 3>>();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        (*combos)[i] = routingCombo();
        audioForm->addRow(labels[i], (*combos)[i]);
    }
    audioLay->addLayout(audioForm);
    vbox->addWidget(audioBox);

    // ---- CW skimmer -----------------------------------------------------------
    auto* skimBox = new QGroupBox(QObject::tr("CW skimmer"));
    auto* skimLay = new QVBoxLayout(skimBox);
    auto* skimIntro = new QLabel(QObject::tr(
        "Decodes the CW segment of the band the radio is on, many signals at once, from "
        "the RSP2. Callsigns it finds (checked against the country file and repeated "
        "before they count) appear as violet spots on the panadapter and are served to "
        "cqrlog and Not1MM as a DX cluster on localhost (point their cluster window at "
        "localhost and the telnet port below)."));
    skimIntro->setWordWrap(true);
    skimLay->addWidget(skimIntro);
    auto* skimEnabled = new QCheckBox(QObject::tr("Skim the CW segment"));
    auto* skimCwOnly = new QCheckBox(QObject::tr("Pause outside CW"));
    skimCwOnly->setToolTip(QObject::tr("Stop decoding while the radio is in SSB, AM or FM; "
                                       "resume as soon as it is back in CW."));
    auto* skimBayes = new QCheckBox(QObject::tr("Bayes decoder (experimental)"));
    skimBayes->setToolTip(QObject::tr("The console's experimental decoder; takes effect at "
                                      "the next connect."));
    skimLay->addWidget(skimEnabled);
    skimLay->addWidget(skimCwOnly);
    skimLay->addWidget(skimBayes);
    auto* skimForm = new QFormLayout;
    auto* skimChannels = new QSpinBox;
    skimChannels->setRange(4, 64);
    auto* skimPort = new QSpinBox;
    skimPort->setRange(0, 65535);
    skimPort->setSpecialValueText(QObject::tr("off"));
    skimForm->addRow(QObject::tr("Decoder channels:"), skimChannels);
    skimForm->addRow(QObject::tr("Telnet port:"), skimPort);
    skimLay->addLayout(skimForm);
    auto* skimNote = new QLabel(QObject::tr(
        "Channels, port and the decoder choice take effect at the next connect. "
        "24 channels use more than one CPU core."));
    skimNote->setWordWrap(true);
    skimLay->addWidget(skimNote);
    auto* skimStatus = new QLabel;
    skimStatus->setWordWrap(true);
    skimLay->addWidget(skimStatus);
    vbox->addWidget(skimBox);

    // ---- The operator, for the CW macros --------------------------------------
    // AetherSDR keeps the station callsign (Settings -> Radio Setup) but has
    // no setting for the operator's name, town or grid, and the CW window's
    // [my name] / [my qth] / [my grid] fields need all three.
    auto* meBox = new QGroupBox(QObject::tr("You, for the CW macros"));
    auto* meLay = new QVBoxLayout(meBox);
    auto* meIntro = new QLabel(QObject::tr(
        "What [my name], [my qth] and [my grid] send in a CW macro (the CW window, "
        "Tools -> CW window). Your callsign comes from the station callsign in Radio "
        "Setup."));
    meIntro->setWordWrap(true);
    meLay->addWidget(meIntro);
    auto* meForm = new QFormLayout;
    const auto meField = [&](const QString& label, const char* key, const QString& hint) {
        auto* e = new QLineEdit(AppSettings::instance().value(QLatin1String(key)).toString());
        e->setPlaceholderText(hint);
        const QString k = QLatin1String(key);
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, k] {
            auto& s = AppSettings::instance();
            s.setValue(k, e->text().trimmed());
            s.save();
        });
        meForm->addRow(label, e);
    };
    meField(QObject::tr("Your name:"), "OrionCw/myName", QObject::tr("as you send it"));
    meField(QObject::tr("Your town:"), "OrionCw/myQth", QObject::tr("as you send it"));
    meField(QObject::tr("Your grid:"), "OrionCw/myGrid", QObject::tr("e.g. EN82"));
    meLay->addLayout(meForm);
    vbox->addWidget(meBox);

    vbox->addStretch(1);

    QPointer<QWidget> pagePtr(page);

    const auto fillSkimmer = [=](const QVariantMap& m) {
        {
            const QSignalBlocker b1(skimEnabled), b2(skimCwOnly), b3(skimBayes),
                b4(skimChannels), b5(skimPort);
            skimEnabled->setChecked(m.value(QStringLiteral("enabled")).toBool());
            skimCwOnly->setChecked(m.value(QStringLiteral("cwOnly"), true).toBool());
            skimBayes->setChecked(m.value(QStringLiteral("bayes")).toBool());
            skimChannels->setValue(m.value(QStringLiteral("channels"), 24).toInt());
            skimPort->setValue(m.value(QStringLiteral("port"), 7300).toInt());
        }
        QString text = QObject::tr("Status: %1").arg(m.value(QStringLiteral("status")).toString());
        if (!m.value(QStringLiteral("available")).toBool()) {
            text += QObject::tr(" (it runs with the RSP2 panadapter)");
        } else {
            text += QObject::tr("; %1 channel(s) decoding")
                        .arg(m.value(QStringLiteral("activeChannels")).toInt());
            const int port = m.value(QStringLiteral("serverPort")).toInt();
            text += port > 0 ? QObject::tr("; telnet feed on localhost:%1, %2 client(s)")
                                   .arg(port)
                                   .arg(m.value(QStringLiteral("serverClients")).toInt())
                             : QObject::tr("; telnet feed off");
        }
        skimStatus->setText(text);
    };

    const auto reseed = [=]() {
        const bool on = orionConnected(model);
        notConnected->setVisible(!on);
        audioBox->setEnabled(on);
        skimBox->setEnabled(on);
        if (!on || !pagePtr) {
            return;
        }
        askOrion(model, page, QStringLiteral("audio.routing.get"), [=](const QVariantMap& m) {
            for (std::size_t i = 0; i < keys.size(); ++i) {
                QComboBox* c = (*combos)[i];
                if (!c) continue;
                const QSignalBlocker block(c);
                const int idx = c->findData(m.value(QLatin1String(keys[i])).toString());
                if (idx >= 0) c->setCurrentIndex(idx);
            }
        });
        askOrion(model, page, QStringLiteral("skimmer.get"), fillSkimmer);
    };

    for (std::size_t i = 0; i < keys.size(); ++i) {
        QComboBox* c = (*combos)[i];
        const char* key = keys[i];
        QObject::connect(c, &QComboBox::currentIndexChanged, page, [model, c, key](int) {
            if (!orionConnected(model)) return;
            QVariantMap fields;
            fields.insert(QLatin1String(key), c->currentData().toString());
            model->invokeBackendExtension(QStringLiteral("orion"),
                                          QStringLiteral("audio.routing.set"), 0, fields);
        });
    }

    const auto setSkimmer = [=](const QString& key, const QVariant& value) {
        if (!orionConnected(model)) return;
        QVariantMap fields;
        fields.insert(key, value);
        model->invokeBackendExtension(QStringLiteral("orion"), QStringLiteral("skimmer.set"),
                                      0, fields);
        askOrion(model, page, QStringLiteral("skimmer.get"), fillSkimmer);
    };
    QObject::connect(skimEnabled, &QCheckBox::toggled, page,
                     [=](bool on) { setSkimmer(QStringLiteral("enabled"), on); });
    QObject::connect(skimCwOnly, &QCheckBox::toggled, page,
                     [=](bool on) { setSkimmer(QStringLiteral("cwOnly"), on); });
    QObject::connect(skimBayes, &QCheckBox::toggled, page,
                     [=](bool on) { setSkimmer(QStringLiteral("bayes"), on); });
    QObject::connect(skimChannels, &QSpinBox::editingFinished, page,
                     [=] { setSkimmer(QStringLiteral("channels"), skimChannels->value()); });
    QObject::connect(skimPort, &QSpinBox::editingFinished, page,
                     [=] { setSkimmer(QStringLiteral("port"), skimPort->value()); });

    // The status line stays live while the page is open.
    auto* tick = new QTimer(page);
    tick->setInterval(2000);
    QObject::connect(tick, &QTimer::timeout, page, [=] {
        if (pagePtr && pagePtr->isVisible() && orionConnected(model)) {
            askOrion(model, page, QStringLiteral("skimmer.get"), fillSkimmer);
        }
    });
    tick->start();

    QObject::connect(model, &RadioModel::connectionStateChanged, page,
                     [reseed](bool) { reseed(); });
    reseed();
    return page;
}

}  // namespace AetherSDR
