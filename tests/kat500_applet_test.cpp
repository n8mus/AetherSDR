#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/Kat500Applet.h"
#include "models/Kat500Model.h"
#include <QApplication>
#include <QCheckBox>
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
    auto* scroll = widget.findChild<QScrollArea*>("katScroll");
    CHECK(scroll && scroll->verticalScrollBar()->maximum() > 0);
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
    CHECK(!widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    AetherSDR::Kat500ModelTest::antennas(model);
    CHECK(widget.findChild<QPushButton*>("katAntenna1")->isEnabled());
    CHECK(!widget.findChild<QPushButton*>("katAntenna2")->isEnabled());
    CHECK(widget.findChild<QPushButton*>("katAntenna2")->accessibleDescription().contains("disabled"));
    auto* capacitance = widget.findChild<QSpinBox*>("katCapacitance");
    capacitance->setValue(1000);
    emit model.availabilityChanged();
    CHECK(capacitance->value() == 1000);
    AetherSDR::Kat500ModelTest::stale(model);
    CHECK(!tune->isEnabled() && widget.findChild<QPushButton*>("katPower")->text() == "ON");
    AetherSDR::Kat500ModelTest::off(model);
    CHECK(widget.findChild<QPushButton*>("katPower")->text() == "OFF" && !tune->isEnabled());
    CHECK(!model.followTxFrequency());
    return 0;
}
