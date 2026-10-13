#include "Kat500Applet.h"
#include "CanonIndicators.h"
#include "CanonWindow.h"
#include "HGauge.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/Kat500Model.h"
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
namespace AetherSDR
{
namespace
{
void availability(QWidget* widget, const QString& reason)
{
    widget->setEnabled(reason.isEmpty());
    widget->setToolTip(reason);
    widget->setAccessibleDescription(reason);
}
QString flag(std::optional<bool> value)
{
    return value ? (*value ? QStringLiteral("ON") : QStringLiteral("OFF")) : QStringLiteral("unknown");
}
QString reading(std::optional<double> value)
{
    return value ? QString::number(*value, 'f', 2) : QStringLiteral("—");
}
void text(QLabel* label, const QString& value)
{
    if (label->text() == value) {
        return;
    }
    label->setText(value);
    label->setAccessibleName(value);
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
void pill(QLabel* label, const QString& value, const QString& detail, const char* tone)
{
    text(label, value);
    label->setToolTip(detail);
    label->setAccessibleDescription(detail);
    const QString state = QString::fromLatin1(tone);
    if (label->property("katTone").toString() != state) {
        label->setProperty("katTone", state);
        label->style()->unpolish(label);
        label->style()->polish(label);
        label->update();
    }
}
QJsonObject preferences()
{
    return QJsonDocument::fromJson(AppSettings::instance().value("Kat500Applet").toString().toUtf8())
        .object();
}
} // namespace
Kat500Applet::Kat500Applet(QWidget* parent) : CanonApplet(parent)
{
    setObjectName("Kat500Applet");
    setAccessibleName(tr("Elecraft KAT500 tuner"));
    setCloseButtonVisible(false);
    theme::setContainer(this, QStringLiteral("canon/kat500"));
    setMinimumWidth(254);
    m_clock.start();
    const QJsonObject prefs = preferences();
    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(bodyWidget());
    m_scroll->setObjectName("katScroll");
    m_scroll->setAccessibleName(tr("Tuner controls and telemetry"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_content = new QWidget;
    m_content->setObjectName("katContent");
    m_scroll->viewport()->setObjectName("katViewport");
    m_scroll->setWidget(m_content);
    body->addWidget(m_scroll);
    auto* root = new QVBoxLayout(m_content);
    root->setContentsMargins(4, 2, 4, 2);
    root->setSpacing(2);
    root->setSizeConstraint(QLayout::SetMinAndMaxSize);
    const auto button = [this](const QString& label, const QString& name, QLayout* layout) {
        auto* b = new QPushButton(label, this);
        b->setObjectName(name);
        b->setAccessibleName(label);
        b->setAutoDefault(false);
        b->setFixedHeight(22);
        layout->addWidget(b);
        return b;
    };
    const auto label = [this](const QString& name, QLayout* layout) {
        auto* l = new QLabel(this);
        l->setObjectName(name);
        l->setWordWrap(true);
        l->setTextFormat(Qt::PlainText);
        l->setAccessibleName(name);
        layout->addWidget(l);
        return l;
    };
    const auto statusPill = [label](const QString& name, QLayout* layout) {
        QLabel* cell = label(name, layout);
        cell->setProperty("katPill", true);
        cell->setWordWrap(false);
        cell->setAlignment(Qt::AlignCenter);
        cell->setFixedHeight(18);
        return cell;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(4);
    m_power = button(tr("—"), "katPower", header);
    m_power->setCheckable(true);
    m_power->setFixedWidth(44);
    m_frequency = statusPill("katFrequency", header);
    header->setStretch(1, 1);
    m_mini = button(tr("Mini"), "katMini", header);
    m_mini->setCheckable(true);
    m_mini->setFixedWidth(38);
    root->addLayout(header);
    auto* tuning = new QHBoxLayout;
    tuning->setSpacing(4);
    m_tune = button(tr("Tune"), "katTune", tuning);
    m_tune->setProperty("aetherTxKeying", true);
    m_tune->setFixedWidth(60);
    m_fault = statusPill("katFault", tuning);
    tuning->setStretch(1, 1);
    auto* clear = button(tr("Clear fault"), "katClearFault", tuning);
    clear->setFixedWidth(72);
    root->addLayout(tuning);
    auto* modes = new QHBoxLayout;
    modes->setSpacing(2);
    const QStringList names{tr("Auto"), tr("Manual"), tr("Bypass")};
    for (int i = 0; i < 3; ++i) {
        m_modes[i] = button(names[i], "kat" + names[i], modes);
        m_modes[i]->setCheckable(true);
        connect(m_modes[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setMode(static_cast<Kat500::Mode>(i));
            }
            refresh();
        });
    }
    root->addLayout(modes);
    auto* ants = new QHBoxLayout;
    ants->setSpacing(2);
    for (int i = 0; i < 3; ++i) {
        m_antennas[i] = button(tr("ANT %1").arg(i + 1), QStringLiteral("katAntenna%1").arg(i + 1), ants);
        m_antennas[i]->setCheckable(true);
        connect(m_antennas[i], &QPushButton::clicked, this, [this, i] {
            if (m_model) {
                m_model->setAntenna(i + 1);
            }
            refresh();
        });
    }
    root->addLayout(ants);
    auto* swrRow = new QHBoxLayout;
    m_swr = statusPill("katSwr", swrRow);
    m_swr->setFixedWidth(82);
    swrRow->setSpacing(4);
    m_swrGauge = new HGauge(1, 5, 5, "", "", {{1, "1"}, {5, "5"}}, this);
    m_swrGauge->setFixedHeight(24);
    swrRow->addWidget(m_swrGauge, 1);
    root->addLayout(swrRow);
    auto* bypassRow = new QHBoxLayout;
    m_bypassSwr = statusPill("katBypassSwr", bypassRow);
    m_bypassSwr->setFixedWidth(82);
    bypassRow->setSpacing(4);
    m_bypassGauge = new HGauge(1, 5, 5, "", "", {{1, "1"}, {5, "5"}}, this);
    m_bypassGauge->setFixedHeight(24);
    bypassRow->addWidget(m_bypassGauge, 1);
    root->addLayout(bypassRow);
    auto* physical = new QWidget(this);
    physical->setObjectName("katPhysicalState");
    auto* states = new QHBoxLayout(physical);
    states->setContentsMargins(0, 0, 0, 0);
    states->setSpacing(3);
    m_physical[0] = statusPill("katBypassedStatus", states);
    m_physical[1] = statusPill("katAttenuatorStatus", states);
    m_physical[2] = statusPill("katAmpInterruptStatus", states);
    m_readiness = statusPill("katReadiness", states);
    root->addWidget(physical);
    auto* tools = new QHBoxLayout;
    tools->setSpacing(3);
    m_manualToggle = button(tr("▸ Manual"), "katManualToggle", tools);
    m_manualToggle->setAccessibleName(tr("Manual controls"));
    m_manualToggle->setCheckable(true);
    m_manualPane = new QWidget(this);
    m_manualPane->setObjectName("katManualPane");
    auto* manual = new QVBoxLayout(m_manualPane);
    manual->setContentsMargins(0, 0, 0, 0);
    auto* explanation = label("katManualHelp", manual);
    explanation->setText(
        tr("Relay and frequency requests change the signal path. Controls show confirmed readback."));
    for (int kind = 0; kind < 2; ++kind) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(kind == 0 ? tr("C (pF)") : tr("L (nH)"), this));
        auto* numeric = new QSpinBox(this);
        numeric->setObjectName(kind == 0 ? "katCapacitance" : "katInductance");
        numeric->setAccessibleName(kind == 0 ? tr("Capacitance in picofarads")
                                             : tr("Inductance in nanohenries"));
        numeric->setRange(0, Kat500::relayTotal(kind == 0, 255));
        row->addWidget(numeric, 1);
        if (kind == 0) {
            m_cap = numeric;
        } else {
            m_ind = numeric;
        }
        auto* request = button(tr("Set"), kind == 0 ? "katSetCapacitance" : "katSetInductance", row);
        manual->addLayout(row);
        connect(request, &QPushButton::clicked, this, [this, numeric, kind] {
            if (m_model) {
                const int mask = Kat500::relayMaskForValue(kind == 0, numeric->value());
                if (kind == 0) {
                    m_model->setCapacitorMask(mask);
                } else {
                    m_model->setInductorMask(mask);
                }
            }
            refresh();
        });
        auto* bits = new QGridLayout;
        for (int bit = 0; bit < 8; ++bit) {
            auto* check = new QCheckBox(QString::number(bit), this);
            check->setObjectName(QStringLiteral("kat%1Bit%2").arg(kind == 0 ? "C" : "L").arg(bit));
            check->setAccessibleName(
                tr("%1 relay bit %2").arg(kind == 0 ? "Capacitor" : "Inductor").arg(bit));
            bits->addWidget(check, bit / 4, bit % 4);
            (kind == 0 ? m_capBits : m_indBits)[bit] = check;
            connect(check, &QCheckBox::clicked, this, [this, kind, bit](bool on) {
                if (m_model) {
                    const auto value =
                        kind == 0 ? m_model->status().capacitorMask : m_model->status().inductorMask;
                    if (value) {
                        const int mask = on ? (*value | (1 << bit)) : (*value & ~(1 << bit));
                        if (kind == 0) {
                            m_model->setCapacitorMask(mask);
                        } else {
                            m_model->setInductorMask(mask);
                        }
                    }
                }
                refresh();
            });
        }
        manual->addLayout(bits);
    }
    m_side = new QCheckBox(tr("Capacitor on antenna side"), this);
    m_side->setObjectName("katCapacitorSide");
    manual->addWidget(m_side);
    connect(m_side, &QCheckBox::clicked, this, [this](bool on) {
        if (m_model) {
            m_model->setCapacitorSide(on ? Kat500::CapacitorSide::Antenna
                                         : Kat500::CapacitorSide::Transmitter);
        }
        refresh();
    });
    const QStringList stateNames{tr("Physical bypass"), tr("Attenuator"), tr("Amp key interrupt")};
    for (int i = 0; i < 3; ++i) {
        auto* c = new QCheckBox(stateNames[i], this);
        c->setObjectName(QStringLiteral("katManualState%1").arg(i));
        c->setAccessibleName(stateNames[i]);
        m_manualStates[i] = c;
        manual->addWidget(c);
        connect(c, &QCheckBox::clicked, this, [this, i](bool on) {
            if (m_model) {
                if (i == 0) {
                    m_model->setBypassed(on);
                } else if (i == 1) {
                    m_model->setAttenuator(on);
                } else {
                    m_model->setAmpKeyInterrupted(on);
                }
            }
            refresh();
        });
    }
    auto* frequencyRow = new QHBoxLayout;
    m_frequencyRecall = new QSpinBox(this);
    m_frequencyRecall->setObjectName("katRecallFrequency");
    m_frequencyRecall->setAccessibleName(tr("Memory frequency in kHz"));
    m_frequencyRecall->setRange(1, 99999);
    manual->addWidget(m_frequencyRecall);
    auto* recall = button(tr("Recall kHz"), "katRecall", frequencyRow);
    auto* memorize = button(tr("Memorize"), "katMemorize", frequencyRow);
    manual->addLayout(frequencyRow);
    auto* ftns = button(tr("Full tune without saving"), "katFullTuneNoSave", manual);
    ftns->setProperty("aetherTxKeying", true);
    connect(recall, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->recallFrequency(m_frequencyRecall->value());
        }
    });
    connect(memorize, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->memorize(m_frequencyRecall->value());
        }
    });
    connect(ftns, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->startTune(false);
        }
    });
    m_historyToggle = button(tr("▸ History"), "katHistory", tools);
    m_settings = button(tr("▸ Settings"), "katSettings", tools);
    root->addLayout(tools);
    root->addWidget(m_manualPane);
    m_historyToggle->setCheckable(true);
    m_settings->setCheckable(true);
    m_historyPane = new QWidget(this);
    auto* history = new QVBoxLayout(m_historyPane);
    history->setContentsMargins(0, 0, 0, 0);
    m_graph = label("katHistoryGraph", history);
    m_graph->setMinimumHeight(270);
    m_graph->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    m_graph->installEventFilter(this);
    root->addWidget(m_historyPane, 1);
    m_settingsPane = new QWidget(this);
    auto* settings = new QVBoxLayout(m_settingsPane);
    settings->setContentsMargins(0, 0, 0, 0);
    m_follow = new QCheckBox(tr("Follow TX frequency"), this);
    m_follow->setObjectName("katFollowTx");
    m_follow->setAccessibleDescription(
        tr("Follow the effective TX slice frequency including split and XIT. This sends frequency requests "
           "only; it does not key a radio. Default off."));
    m_follow->setChecked(prefs.value("FollowTx").toBool(false));
    settings->addWidget(m_follow);
    m_identity = label("katIdentity", settings);
    m_availability = label("katAvailability", settings);
    m_result = label("katOperationResult", settings);
    auto* help = label("katHistoryHelp", settings);
    help->setText(tr("History: SWR and forward/reflected ADC counts, last observed, 5 minutes. Gaps mean "
                     "missing replies; ADC counts are not watts. SWR may describe an earlier RF measurement. "
                     "Debug TX means serial writes, not RF."));
    auto* advanced = button(tr("Open Advanced settings"), "katAdvancedSettings", settings);
    connect(advanced, &QPushButton::clicked, this, &Kat500Applet::advancedSettingsRequested);
    m_debugToggle = button(tr("▸ Debug"), "katDebug", settings);
    m_debugToggle->setCheckable(true);
    m_debugPane = new QWidget(this);
    auto* debug = new QVBoxLayout(m_debugPane);
    debug->setContentsMargins(0, 0, 0, 0);
    m_verbose = new QCheckBox(tr("Verbose"), this);
    m_verbose->setObjectName("katVerbose");
    m_verbose->setChecked(prefs.value("Verbose").toBool(false));
    debug->addWidget(m_verbose);
    auto* debugActions = new QHBoxLayout;
    m_pause = new QCheckBox(tr("Pause display"), this);
    debug->addWidget(m_pause);
    auto* copy = button(tr("Copy"), "katDebugCopy", debugActions);
    auto* save = button(tr("Save…"), "katDebugSave", debugActions);
    auto* clearDebug = button(tr("Clear"), "katDebugClear", debugActions);
    debug->addLayout(debugActions);
    m_debug = new QPlainTextEdit(this);
    m_debug->setObjectName("katDebugOutput");
    m_debug->setAccessibleName(tr("Tuner protocol diagnostics"));
    m_debug->setReadOnly(true);
    m_debug->setMaximumBlockCount(1000);
    m_debug->setFixedHeight(160);
    debug->addWidget(m_debug);
    settings->addWidget(m_debugPane);
    root->addWidget(m_settingsPane);
    root->addStretch();
    connect(m_power, &QPushButton::clicked, this, [this] {
        if (m_model && m_model->status().powerOn) {
            m_model->setPower(!*m_model->status().powerOn);
        }
        refresh();
    });
    connect(m_tune, &QPushButton::clicked, this, [this] {
        if (m_model) {
            if (m_model->status().tuning.value_or(false)) {
                m_model->cancelTune();
            } else {
                m_model->startTune();
            }
        }
        refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->clearFault();
        }
    });
    for (auto* b : {m_mini, m_manualToggle, m_settings, m_historyToggle, m_debugToggle}) {
        connect(b, &QPushButton::toggled, this, [this] {
            savePreferences();
            presentation();
        });
    }
    connect(m_follow, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setFollowTxFrequency(on);
        }
        savePreferences();
        emit followTxChanged(on);
    });
    connect(m_verbose, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setVerbose(on);
        }
        savePreferences();
    });
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_debug->toPlainText()); });
    connect(save, &QPushButton::clicked, this, &Kat500Applet::saveDebug);
    connect(clearDebug, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->clearDebug();
        }
    });
    m_mini->setChecked(prefs.value("MiniMode").toBool(false));
    m_historyToggle->setChecked(prefs.value("HistoryVisible").toBool(false));
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &Kat500Applet::refresh);
    m_timer.start();
    applyCanonSheet(bodyWidget(), [] {
        return QStringLiteral(
            "QWidget#katContent, QWidget#katViewport, QScrollArea#katScroll { background: {{color.canon.ground}}; border: none; }"
            "QLabel, QCheckBox { color: {{color.canon.muted}}; font-size: 10px; }"
            "QLabel[katPill=true] { font-size: 9px; font-weight: bold; background: {{color.canon.nested}}; "
            "border: 1px solid {{color.canon.lineHi}}; border-radius: 3px; padding: 0 2px; }"
            "QLabel[katTone=ok] { color: {{color.accent.success}}; }"
            "QLabel[katTone=warn] { color: {{color.accent.warning}}; }"
            "QLabel[katTone=error] { color: {{color.accent.danger}}; }"
            "QPushButton { padding: 0 4px; font-size: 10px; font-weight: bold; "
            "color: {{color.canon.inkSoft}}; background: {{color.canon.control}}; "
            "border-radius: 3px; border: 1px solid {{color.canon.lineHi}}; } "
            "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; } "
            "QSpinBox { font-size: 10px; min-height: 18px; } "
            "QPlainTextEdit { font-size: 10px; font-family: monospace; color: {{color.canon.inkSoft}}; } "
            "QPushButton:checked:enabled { background: "
            "{{color.accessory.key.meffa.active.background}}; color: "
            "{{color.accessory.key.meffa.active.foreground}}; } QPushButton#katBypass:checked:enabled { "
            "background: {{color.accessory.key.standby.background}}; color: "
            "{{color.accessory.key.standby.foreground}}; } QPushButton:disabled { color: "
            "{{color.text.secondary}}; background: {{color.background.2}}; } "
            "QPushButton:checked:disabled { background: {{color.accessory.key.meffa.active.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.meffa.active.border}}; } "
            "QPushButton#katBypass:checked:disabled { background: {{color.accessory.key.standby.background}}; "
            "color: {{color.text.secondary}}; border-color: {{color.accessory.key.standby.foreground}}; }");
    });
    presentation();
    refresh();
}
Kat500Applet::~Kat500Applet()
{
    if (m_fileDialog) {
        QFileDialog* dialog = m_fileDialog.data();
        m_fileDialog.clear();
        disconnect(dialog, nullptr, this, nullptr);
        dialog->reject();
        // Let the native sheet finish closing before QObject destruction.
        dialog->setParent(nullptr);
        dialog->deleteLater();
    }
}
void Kat500Applet::setModel(Kat500Model* model)
{
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (model) {
        model->setFollowTxFrequency(m_follow->isChecked());
        model->setVerbose(m_verbose->isChecked());
        connect(model, &Kat500Model::statusUpdated, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::operationFinished, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kat500Model::operationFailed, this,
                [this](const QString& message) { text(m_result, message); });
        connect(model, &Kat500Model::availabilityChanged, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::configurationRead, this, &Kat500Applet::refresh);
        connect(model, &Kat500Model::debugChanged, this, [this] {
            if (m_model && !m_pause->isChecked()) {
                m_debug->setPlainText(m_model->debugText());
            }
        });
        connect(model, &Kat500Model::telemetryReceived, this,
                [this](const QString& command, const Kat500::Status& state) {
                    const int i = command == "VSWR" ? 0 : command == "VFWD" ? 1 : command == "VRFL" ? 2 : -1;
                    if (i < 0) {
                        return;
                    }
                    std::optional<double> value;
                    if (i == 0) {
                        value = state.swr;
                    } else {
                        const auto count = i == 1 ? state.forwardAdc : state.reflectedAdc;
                        if (count) {
                            value = *count;
                        }
                    }
                    m_history[i].push_back({m_clock.elapsed(), value});
                    while (m_history[i].size() > 4096) {
                        m_history[i].pop_front();
                    }
                });
        connect(model, &Kat500Model::disconnected, this, [this] {
            for (auto& series : m_history) {
                series.push_back({m_clock.elapsed(), std::nullopt});
            }
            refresh();
        });
    }
    refresh();
}
void Kat500Applet::refresh()
{
    const Kat500::Status s = m_model ? m_model->status() : Kat500::Status{};
    const QString reason = m_model ? m_model->unavailableReason(true) : tr("Connect a KAT500 first.");
    const bool ready = reason.isEmpty();
    const bool tuning = s.tuning.value_or(false);
    text(m_frequency, tr("%1 · %2 kHz")
                          .arg(s.band ? Kat500::bandName(*s.band) : tr("unknown band"),
                               s.frequencyKhz ? QString::number(*s.frequencyKhz) : QStringLiteral("—")));
    m_power->setText(s.powerOn ? (*s.powerOn ? tr("ON") : tr("OFF")) : QStringLiteral("—"));
    m_power->setChecked(s.powerOn.value_or(false));
    m_power->setAccessibleName(tr("Tuner power, confirmed %1").arg(m_power->text()));
    availability(m_power, m_model && m_model->isDetected() && m_model->isResponding() &&
                                  !m_model->operationBusy() && s.powerOn
                              ? QString()
                              : tr("Wait for confirmed tuner identity, power and an idle operation."));
    m_tune->setText(tuning ? tr("Cancel") : tr("Tune"));
    m_tune->setAccessibleName(m_tune->text());
    availability(m_tune, tuning && m_model && m_model->isResponding() ? QString() : reason);
    for (int i = 0; i < 3; ++i) {
        m_modes[i]->setChecked(s.mode && static_cast<int>(*s.mode) == i);
        availability(m_modes[i], reason);
        m_antennas[i]->setChecked(s.antenna == i + 1);
        QString antennaReason = reason;
        if (antennaReason.isEmpty()) {
            if (!s.band ||
                !m_model->configuration().contains({Kat500::Setting::EnabledAntenna, *s.band, i + 1})) {
                antennaReason =
                    tr("Read Advanced configuration to confirm antenna availability for this band.");
            } else if (m_model->configuration().value({Kat500::Setting::EnabledAntenna, *s.band, i + 1}) ==
                       0) {
                antennaReason = tr("This antenna is disabled on the confirmed band.");
            }
        }
        availability(m_antennas[i], antennaReason);
    }
    availability(findChild<QPushButton*>("katClearFault"), reason);
    pill(m_fault, s.faultCode ? (*s.faultCode == 0 ? tr("NO FAULT") : tr("FAULT %1").arg(*s.faultCode))
                             : tr("FAULT ?"),
         s.faultCode ? Kat500::currentFaultName(*s.faultCode) : tr("Fault unknown"),
         s.faultCode ? (*s.faultCode == 0 ? "ok" : "error") : "muted");
    text(m_swr, tr("SWR %1").arg(reading(s.swr)));
    text(m_bypassSwr, tr("BYP SWR %1").arg(reading(s.bypassSwr)));
    m_swrGauge->setValue(s.swr.value_or(1));
    m_bypassGauge->setValue(s.bypassSwr.value_or(1));
    const std::array<std::optional<bool>, 3> physical{s.bypassed, s.attenuator, s.ampKeyInterrupted};
    const QStringList shortNames{tr("BYP"), tr("ATT"), tr("KEY")};
    const QStringList fullNames{tr("Physical bypass"), tr("Attenuator"), tr("Amplifier key interrupt")};
    for (int i = 0; i < 3; ++i) {
        pill(m_physical[i], tr("%1 %2").arg(shortNames[i], physical[i] ? flag(physical[i]) : tr("?")),
             tr("%1: %2").arg(fullNames[i], flag(physical[i])),
             physical[i].value_or(false) ? "warn" : "muted");
    }
    const bool responding = m_model && m_model->isResponding();
    const QString readiness = !m_model || !m_model->isDetected() ? tr("OFFLINE")
                              : !responding ? tr("STALE")
                              : s.powerOn == false ? tr("OFF")
                              : tuning ? tr("TUNING")
                              : ready ? tr("READY") : tr("WAIT");
    pill(m_readiness, readiness, ready ? tr("Tuner is ready for an operator request.") : reason,
         ready ? "ok" : "warn");
    text(m_availability, tr("%1\nLast observed SWR: %2 ms since reply.")
                             .arg(ready ? tr("Tuner ready.") : reason)
                             .arg(m_model ? m_model->fieldAgeMs("VSWR") : -1));
    text(m_identity, m_model
                         ? tr("%1\n%2 · Firmware %3 · S/N %4\nReply %5 · forward/reflected ADC %6 / %7")
                               .arg(m_model->description(), s.identity, s.firmware, s.serial,
                                    m_model->isResponding() ? tr("fresh") : tr("stale"),
                                    s.forwardAdc ? QString::number(*s.forwardAdc) : QStringLiteral("—"),
                                    s.reflectedAdc ? QString::number(*s.reflectedAdc) : QStringLiteral("—"))
                         : tr("No tuner connected."));
    for (auto* control : m_manualPane->findChildren<QWidget*>()) {
        if (qobject_cast<QPushButton*>(control) || qobject_cast<QSpinBox*>(control) ||
            qobject_cast<QCheckBox*>(control)) {
            availability(control, reason);
        }
    }
    for (int kind = 0; kind < 2; ++kind) {
        const auto mask = kind == 0 ? s.capacitorMask : s.inductorMask;
        auto* numeric = kind == 0 ? m_cap : m_ind;
        const QSignalBlocker blocked(numeric);
        if (mask && mask != m_lastRelayMasks[kind]) {
            numeric->setValue(Kat500::relayTotal(kind == 0, *mask));
        }
        m_lastRelayMasks[kind] = mask;
        for (int bit = 0; bit < 8; ++bit) {
            auto* check = (kind == 0 ? m_capBits : m_indBits)[bit];
            const QSignalBlocker b(check);
            check->setChecked(mask && (*mask & (1 << bit)));
            if (!mask) {
                availability(check, tr("Wait for a confirmed relay mask."));
            }
        }
    }
    {
        const QSignalBlocker b(m_side);
        m_side->setChecked(s.side == Kat500::CapacitorSide::Antenna);
        if (!s.side) {
            availability(m_side, tr("Wait for confirmed capacitor side."));
        }
    }
    const std::array<std::optional<bool>, 3> values{s.bypassed, s.attenuator, s.ampKeyInterrupted};
    for (int i = 0; i < 3; ++i) {
        const QSignalBlocker b(m_manualStates[i]);
        m_manualStates[i]->setChecked(values[i].value_or(false));
        if (!values[i]) {
            availability(m_manualStates[i], tr("Wait for confirmed state."));
        }
    }
    if (reason.isEmpty() && s.firmware != QStringLiteral("02.16")) {
        availability(findChild<QPushButton*>("katMemorize"),
                     tr("Stored match verification supports firmware 02.16 only."));
    }
    if (m_historyPane->isVisible()) {
        renderHistory();
    }
}
void Kat500Applet::presentation()
{
    const bool mini = m_mini->isChecked();
    m_mini->setText(mini ? tr("Full") : tr("Mini"));
    m_manualToggle->setVisible(!mini);
    m_manualPane->setVisible(!mini && m_manualToggle->isChecked());
    m_settings->setVisible(!mini);
    m_historyToggle->setVisible(!mini);
    m_settingsPane->setVisible(!mini && m_settings->isChecked());
    m_historyPane->setVisible(!mini && m_historyToggle->isChecked());
    m_debugPane->setVisible(m_debugToggle->isChecked());
    updateGeometry();
}
void Kat500Applet::savePreferences()
{
    QJsonObject p;
    p["MiniMode"] = m_mini->isChecked();
    p["HistoryVisible"] = m_historyToggle->isChecked();
    p["FollowTx"] = m_follow->isChecked();
    p["Verbose"] = m_verbose->isChecked();
    AppSettings::instance().setValue("Kat500Applet",
                                     QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}
void Kat500Applet::setFloating(bool floating)
{
    m_floating = floating;
    updateGeometry();
}
QSize Kat500Applet::sizeHint() const
{
    return QSize(300, std::min(700, m_content->sizeHint().height() + 2));
}
QSize Kat500Applet::minimumSizeHint() const
{
    return QSize(254, 220);
}
bool Kat500Applet::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_graph && event->type() == QEvent::Resize) {
        renderHistory();
    }
    return CanonApplet::eventFilter(watched, event);
}
void Kat500Applet::renderHistory()
{
    const QSize size = m_graph->size();
    if (size.isEmpty()) {
        return;
    }
    QPixmap pixmap(size);
    auto& theme = ThemeManager::instance();
    pixmap.fill(theme.color("color.background.0"));
    QPainter p(&pixmap);
    const QStringList names{tr("SWR (last observed)"), tr("Forward ADC counts"), tr("Reflected ADC counts")};
    const qint64 now = m_clock.elapsed();
    for (int i = 0; i < 3; ++i) {
        const QRectF area(42, i * size.height() / 3.0 + 22, size.width() - 48, size.height() / 3.0 - 28);
        p.setPen(theme.color("color.text.secondary"));
        p.drawText(4, i * size.height() / 3 + 15, names[i]);
        p.drawRect(area);
        double maximum = i == 0 ? 5 : 1;
        if (i != 0) {
            for (const Point& point : m_history[i]) {
                if (point.value) {
                    maximum = std::max(maximum, *point.value);
                }
            }
        }
        QPainterPath path;
        bool segment = false;
        qint64 prior = -10000;
        for (const Point& point : m_history[i]) {
            if (point.time < now - 300000) {
                continue;
            }
            if (!point.value) {
                segment = false;
                continue;
            }
            const QPointF pos(area.left() + area.width() * (point.time - (now - 300000)) / 300000.0,
                              area.bottom() - area.height() * std::clamp(*point.value / maximum, 0.0, 1.0));
            if (!segment || point.time - prior > 5000) {
                path.moveTo(pos);
            } else {
                path.lineTo(pos);
            }
            segment = true;
            prior = point.time;
        }
        p.setPen(QPen(theme.color(this, i == 0 ? "color.canon.cyan" : "color.canon.aqua"), 1.5));
        p.drawPath(path);
    }
    m_graph->setPixmap(pixmap);
}
void Kat500Applet::saveDebug()
{
    if (m_fileDialog) {
        m_fileDialog->raise();
        return;
    }
    auto* dialog = new QFileDialog(this);
    m_fileDialog = dialog;
    dialog->setObjectName("katDebugFileDialog");
    dialog->setWindowTitle(tr("Save KAT500 debug log"));
    dialog->setNameFilter(tr("Text files (*.txt)"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->selectFile("kat500-debug.txt");
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        const QStringList paths = dialog->selectedFiles();
        m_fileDialog.clear();
        dialog->deleteLater();
        if (result != QDialog::Accepted || paths.isEmpty()) {
            return;
        }
        QSaveFile file(paths.first());
        const QByteArray data = m_debug->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            text(m_availability, tr("Debug save failed: %1").arg(file.errorString()));
        }
    });
    dialog->open();
}
} // namespace AetherSDR
