#include "gui/orion/OrionAudioPage.h"

#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <QVariantMap>

#include <array>

namespace AetherSDR {

namespace {

// Request ids are not allocated centrally (RadioSetupDialog's note); this
// decade is the fork's own so it cannot collide with the dialog's counters.
constexpr quint64 kRoutingRequestIdBase = 0x0700000000000000ull;

QComboBox* routingCombo()
{
    auto* combo = new QComboBox;
    combo->addItem(QObject::tr("VFO A (main)"), QStringLiteral("M"));
    combo->addItem(QObject::tr("VFO B (sub)"), QStringLiteral("S"));
    combo->addItem(QObject::tr("Both"), QStringLiteral("B"));
    return combo;
}

}  // namespace

QWidget* buildOrionAudioPage(RadioModel* model)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    vbox->setSpacing(8);

    auto* intro = new QLabel(QObject::tr(
        "Which receiver you hear on the Orion's own outputs: each headphone side and "
        "the speaker can carry VFO A, VFO B or both. This is the radio's setting; it "
        "stays in the radio when AetherSDR closes."));
    intro->setWordWrap(true);
    vbox->addWidget(intro);

    auto* status = new QLabel;
    status->setWordWrap(true);
    vbox->addWidget(status);

    auto* form = new QFormLayout;
    const std::array<const char*, 3> keys{"left", "right", "speaker"};
    const std::array<QString, 3> labels{QObject::tr("Headphones left:"),
                                        QObject::tr("Headphones right:"),
                                        QObject::tr("Speaker:")};
    auto combos = std::make_shared<std::array<QPointer<QComboBox>, 3>>();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        (*combos)[i] = routingCombo();
        form->addRow(labels[i], (*combos)[i]);
    }
    vbox->addLayout(form);
    vbox->addStretch(1);

    const auto setEnabled = [combos](bool on) {
        for (const auto& c : *combos) {
            if (c) c->setEnabled(on);
        }
    };

    // Read the radio's current routing into the combos.
    const auto reseed = [model, combos, status, setEnabled, keys]() {
        IRadioBackend* backend = model ? model->backend() : nullptr;
        if (!backend || !model->isConnected()
            || !model->backendDeclaresExtension(QStringLiteral("orion"))) {
            status->setText(QObject::tr("Connect the Orion to change its audio routing."));
            setEnabled(false);
            return;
        }
        static quint64 nextId = kRoutingRequestIdBase;
        const quint64 requestId = nextId++;
        auto* guard = new QObject(status);
        QObject::connect(backend, &IRadioBackend::extensionResult, guard,
                         [guard, requestId, combos, status, setEnabled, keys](
                             quint64 id, const QVariant& result) {
            if (id != requestId) return;
            guard->deleteLater();
            const QVariantMap m = result.toMap();
            if (m.isEmpty()) {
                status->setText(QObject::tr("The radio has not reported its routing yet; "
                                            "choices made here are sent to it."));
            } else {
                status->clear();
            }
            for (std::size_t i = 0; i < keys.size(); ++i) {
                QComboBox* c = (*combos)[i];
                if (!c) continue;
                const QSignalBlocker block(c);
                const int idx = c->findData(m.value(QLatin1String(keys[i])).toString());
                if (idx >= 0) c->setCurrentIndex(idx);
            }
            setEnabled(true);
        });
        // Armed before the invoke: the reply is emitted synchronously.
        model->invokeBackendExtension(QStringLiteral("orion"),
                                      QStringLiteral("audio.routing.get"), requestId);
    };

    for (std::size_t i = 0; i < keys.size(); ++i) {
        QComboBox* c = (*combos)[i];
        const char* key = keys[i];
        QObject::connect(c, &QComboBox::currentIndexChanged, page, [model, c, key](int) {
            if (!model || !model->backendDeclaresExtension(QStringLiteral("orion"))) return;
            QVariantMap fields;
            fields.insert(QLatin1String(key), c->currentData().toString());
            model->invokeBackendExtension(QStringLiteral("orion"),
                                          QStringLiteral("audio.routing.set"), 0, fields);
        });
    }
    QObject::connect(model, &RadioModel::connectionStateChanged, page,
                     [reseed](bool) { reseed(); });
    reseed();
    return page;
}

}  // namespace AetherSDR
