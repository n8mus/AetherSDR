#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/Kat500Applet.h"
#include "models/Kat500Model.h"
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QPointer>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <cstdio>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                             \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
namespace AetherSDR
{
class Kat500ModelTest
{
  public:
    static void ready(Kat500Model& model)
    {
        model.m_now = [] { return qint64(100); };
        model.m_connected = true;
        model.m_detected = true;
        model.m_lastReply = 100;
        model.m_status.identity = "KAT500";
        model.m_status.powerOn = true;
        model.m_status.tuning = false;
        model.m_status.forwardAdc = 0;
        model.m_status.band = 5;
        model.m_status.antenna = 1;
        model.m_status.mode = Kat500::Mode::Auto;
        model.m_status.capacitorMask = 4;
        model.m_status.frequencyKhz = 14096;
        model.m_status.swr = 1.33;
        model.m_status.bypassSwr = 1.0;
        model.m_status.faultCode = 0;
        model.m_status.bypassed = false;
        model.m_status.attenuator = false;
        model.m_status.ampKeyInterrupted = false;
        model.m_received["TP"] = 100;
        model.m_received["VFWD"] = 100;
    }
    static void antennas(Kat500Model& model)
    {
        for (int i = 1; i <= 3; ++i) {
            model.m_configuration[{Kat500::Setting::EnabledAntenna, 5, i}] = i == 2 ? 0 : 1;
        }
        emit model.configurationRead(model.m_configuration);
    }
    static void blocked(Kat500Model& model, int condition)
    {
        model.m_status.forwardAdc = condition == 0 ? 1 : 0;
        model.m_operation = condition == 1 ? QStringLiteral("Test maintenance") : QString();
        model.m_status.tuning = condition == 2;
        emit model.availabilityChanged();
    }
    static void stale(Kat500Model& model)
    {
        model.m_lastReply = -10000;
        emit model.availabilityChanged();
    }
    static void off(Kat500Model& model)
    {
        model.m_lastReply = 100;
        model.m_status.powerOn = false;
        emit model.statusUpdated(model.m_status);
    }
};
} // namespace AetherSDR
int main(int argc, char** argv)
{
    TestSettingsProfile profile("kat500-applet-test");
    QApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    AetherSDR::Kat500Applet widget;
    widget.resize(310, 300);
    widget.show();
    auto* mini = widget.findChild<QPushButton*>("katMini");
    CHECK(mini);
    auto* tune = widget.findChild<QPushButton*>("katTune");
    CHECK(tune && tune->property("aetherTxKeying").toBool());
    CHECK(!tune->isEnabled() && !tune->accessibleDescription().isEmpty());
    auto* follow = widget.findChild<QCheckBox*>("katFollowTx");
    CHECK(follow && !follow->isChecked());
    auto* scroll = widget.findChild<QScrollArea*>("katScroll");
    CHECK(scroll);
    for (const int width : {238, 248, 254, 300, 360}) {
        widget.resize(width, 220);
        CHECK(widget.width() == width);
        QTest::qWait(30);
        CHECK(scroll->verticalScrollBar()->maximum() == 0);
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
        for (QPushButton* button : widget.findChildren<QPushButton*>()) {
            if (button->isVisible()) {
                CHECK(button->height() <= 22);
                CHECK(button->font().pixelSize() <= 11);
            }
        }
        CHECK(!widget.findChild<QLabel*>("katAvailability")->isVisible());
        CHECK(!widget.findChild<QLabel*>("katOperationResult")->isVisible());
    }
    mini->setChecked(true);
    QTest::qWait(20);
    for (const char* name :
         {"katTune", "katAuto", "katManual", "katBypass", "katAntenna1", "katAntenna2", "katAntenna3",
          "katClearFault", "katSwr", "katBypassSwr", "katPhysicalState", "katFrequency"}) {
        auto* control = widget.findChild<QWidget*>(name);
        CHECK(control && control->isVisible());
    }
    CHECK(!widget.findChild<QWidget*>("katManualPane")->isVisible());
    mini->setChecked(false);
    widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
    widget.findChild<QPushButton*>("katSettings")->setChecked(true);
    widget.findChild<QPushButton*>("katDebug")->setChecked(true);
    QTest::qWait(20);
    CHECK(scroll && scroll->verticalScrollBar()->maximum() > 0);
    for (const int width : {238, 248, 254, 280}) {
        widget.resize(width, 300);
        QTest::qWait(20);
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    }
    widget.resize(700, 1400);
    QTest::qWait(20);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    QSignalSpy advanced(&widget, &AetherSDR::Kat500Applet::advancedSettingsRequested);
    widget.findChild<QPushButton*>("katAdvancedSettings")->click();
    CHECK(advanced.size() == 1);
    AetherSDR::Kat500Model model;
    AetherSDR::Kat500ModelTest::ready(model);
    widget.setModel(&model);
    CHECK(widget.findChild<QPushButton*>("katPower")->text() == "ON" && tune->isEnabled());
    CHECK(widget.findChild<QLabel*>("katReadiness")->text() == "READY");
    CHECK(widget.findChild<QLabel*>("katAvailability")->isVisible());
    CHECK(widget.findChild<QLabel*>("katAvailability")->text().contains("ms since reply"));
    CHECK(!widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    AetherSDR::Kat500ModelTest::antennas(model);
    CHECK(widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    CHECK(!widget.findChild<QPushButton*>("katAntenna2")->isEnabled());
    CHECK(widget.findChild<QPushButton*>("katAntenna2")->accessibleDescription().contains("disabled"));
    for (const char* name : {"katManualToggle", "katSettings", "katHistory"}) {
        widget.findChild<QPushButton*>(name)->setChecked(false);
    }
    widget.resize(300, 220);
    QTest::qWait(20);
    CHECK(scroll->verticalScrollBar()->maximum() == 0);
    const QString capture = qEnvironmentVariable("AETHER_KAT500_GRAB");
    if (!capture.isEmpty()) {
        CHECK(widget.grab().save(capture + "/applet-compact.png"));
    }
    widget.findChild<QPushButton*>("katManualToggle")->setChecked(true);
    widget.findChild<QPushButton*>("katSettings")->setChecked(true);
    widget.resize(700, 1400);
    auto* capacitance = widget.findChild<QSpinBox*>("katCapacitance");
    capacitance->setValue(1000);
    emit model.availabilityChanged();
    CHECK(capacitance->value() == 1000);
    widget.findChild<QPushButton*>("katHistory")->setChecked(true);
    AetherSDR::Kat500::Status sample = model.status();
    sample.swr = 1;
    sample.forwardAdc = 1;
    sample.reflectedAdc = 1;
    for (const QString& command : {QStringLiteral("VSWR"), QStringLiteral("VFWD"), QStringLiteral("VRFL")}) {
        emit model.telemetryReceived(command, sample);
    }
    QTest::qWait(20);
    sample.swr = 5;
    sample.forwardAdc = 250;
    sample.reflectedAdc = 250;
    for (const QString& command : {QStringLiteral("VSWR"), QStringLiteral("VFWD"), QStringLiteral("VRFL")}) {
        emit model.telemetryReceived(command, sample);
    }
    emit model.availabilityChanged();
    const QImage image = widget.findChild<QLabel*>("katHistoryGraph")->pixmap().toImage();
    const QColor cyan = AetherSDR::ThemeManager::instance().color(&widget, "color.canon.cyan");
    const QColor aqua = AetherSDR::ThemeManager::instance().color(&widget, "color.canon.aqua");
    int cyanPixels = 0, aquaPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            cyanPixels += image.pixelColor(x, y) == cyan;
            aquaPixels += image.pixelColor(x, y) == aqua;
        }
    }
    const bool visibleTraces = cyanPixels > 10 && aquaPixels > 10;
    if (!visibleTraces) {
        std::fprintf(stderr, "FAIL missing visible history traces: cyan=%d aqua=%d\n", cyanPixels,
                     aquaPixels);
    }
    for (int condition = 0; condition < 3; ++condition) {
        AetherSDR::Kat500ModelTest::blocked(model, condition);
        auto* selectedMode = widget.findChild<QPushButton*>("katAuto");
        auto* selectedAntenna = widget.findChild<QPushButton*>("katAntenna1");
        CHECK(selectedMode->isChecked() && selectedAntenna->isChecked());
        CHECK(!selectedMode->isEnabled() && !selectedAntenna->isEnabled());
        // Render each disabled button in both states: selection must change its
        // background/border as well as retaining the checked property.
        for (QPushButton* selected : {selectedMode, selectedAntenna}) {
            const QImage checked = selected->grab().toImage();
            selected->setChecked(false);
            const QImage unchecked = selected->grab().toImage();
            selected->setChecked(true);
            const QColor selectedFill = AetherSDR::ThemeManager::instance().color(
                selected, "color.accessory.key.meffa.active.background");
            int changed = 0, selectedPixels = 0;
            for (int y = 0; y < checked.height(); ++y) {
                for (int x = 0; x < checked.width(); ++x) {
                    changed += checked.pixel(x, y) != unchecked.pixel(x, y);
                    selectedPixels += checked.pixelColor(x, y) == selectedFill;
                }
            }
            if (selectedPixels <= checked.width()) {
                std::fprintf(stderr, "selection %s: changed=%d selected=%d expected=%s center=%s\n",
                             qPrintable(selected->objectName()), changed, selectedPixels,
                             qPrintable(selectedFill.name()), qPrintable(checked.pixelColor(10,10).name()));
            }
            CHECK(changed > checked.width() && selectedPixels > checked.width());
        }
    }
    AetherSDR::Kat500ModelTest::blocked(model, 3);
    AetherSDR::Kat500ModelTest::stale(model);
    CHECK(!tune->isEnabled() && widget.findChild<QPushButton*>("katPower")->text() == "ON");
    AetherSDR::Kat500ModelTest::off(model);
    CHECK(widget.findChild<QPushButton*>("katPower")->text() == "OFF" && !tune->isEnabled());
    CHECK(!model.followTxFrequency());
    CHECK(widget.findChild<QPushButton*>("katAuto")->isChecked() &&
          widget.findChild<QPushButton*>("katAntenna1")->isChecked());
    auto* transient = new AetherSDR::Kat500Applet;
    transient->show();
    transient->findChild<QPushButton*>("katSettings")->setChecked(true);
    transient->findChild<QPushButton*>("katDebug")->setChecked(true);
    transient->findChild<QPushButton*>("katDebugSave")->click();
    QTest::qWait(250);
    QPointer<QFileDialog> dialog = transient->findChild<QFileDialog*>("katDebugFileDialog");
    CHECK(dialog);
    delete transient;
    CHECK(dialog && !dialog->isVisible() && !dialog->parent());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::qWait(250);
    CHECK(dialog.isNull());
    return visibleTraces ? 0 : 1;
}
