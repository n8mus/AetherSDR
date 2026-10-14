#include "TestSettingsProfile.h"
#include "core/TxKeyingMarker.h"
#include "core/ThemeManager.h"
#include <QPalette>
#include "gui/TxApplet.h"
#include "gui/AtuPreTuneDialog.h"
#include "models/Kat500Model.h"
#include "models/Kat500TxController.h"
#include "models/RadioModel.h"
#include "models/BandPlanManager.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QTest>
#include <cstdio>

using namespace AetherSDR;
namespace AetherSDR {
class TxAppletPowerReconciliationTestAccess {
public:
    static void prepare(AtuPreTuneDialog& dialog, const std::shared_ptr<TxController>& input) {
        dialog.m_programController = input;
        dialog.m_programInput = input->captureProgram(dialog.usesKat500()
            ? TxController::Activity::Tune : TxController::Activity::Atu);
        dialog.m_sweepActive = true;
        dialog.m_currentIndex = 0;
        dialog.m_txSliceId = 0;
        dialog.m_points = {{"20m", 14.1, 1, 1, 14.0, 14.35}};
        dialog.requestTuneNow();
    }
    static QVector<double> centers(const AtuPreTuneDialog& dialog, const QString& name) {
        for (const auto& row : dialog.m_bands) { if (row.name == name) { return row.centers; } }
        return {};
    }
    static void twoPoints(AtuPreTuneDialog& dialog) {
        dialog.m_mode = AtuPreTuneDialog::Mode::Auto;
        dialog.m_points.append({"20m", 14.12, 2, 2, 14.0, 14.35});
    }
    static void pause(AtuPreTuneDialog& dialog) {
        dialog.m_settleTimer->stop();
        dialog.m_waitingForAtu = false;
    }
    static void settle(AtuPreTuneDialog& dialog) {
        dialog.m_settleTimer->stop();
        QMetaObject::invokeMethod(dialog.m_settleTimer, "timeout", Qt::DirectConnection);
    }
    static void cancel(AtuPreTuneDialog& dialog) { dialog.cancelProgram(); }
    static bool active(const AtuPreTuneDialog& dialog) { return dialog.m_sweepActive; }
    static int successes(const AtuPreTuneDialog& dialog) { return dialog.m_successCount; }
};
// Inject only the serial transport/clock. Production parsing and operations run unchanged.
class Kat500ModelTest {
public:
    static void ready(Kat500Model& k, QList<QByteArray>& wire) {
        k.m_timer.stop(); k.m_now=[] { return qint64(0); };
        k.m_write=[&wire](const QByteArray& bytes) { wire.append(bytes); return true; };
        k.m_nextPoll=60000;
        k.m_connected=true; k.m_detected=true; k.m_lastIo=k.m_lastReply=0;
        k.m_status.identity="KAT500"; k.m_status.serial="00222"; k.m_status.firmware="02.16";
        k.m_status.powerOn=true; k.m_status.tuning=false; k.m_status.forwardAdc=0;
        k.m_status.mode=Kat500::Mode::Auto; k.m_status.faultCode=0; k.m_status.antenna=1;
        for(const QString& field : {"TP","VFWD","MD","PS","FLT"}) { k.m_received[field]=0; }
    }
    static void mode(Kat500Model& k, Kat500::Mode value) {
        k.m_status.mode=value; emit k.statusUpdated(k.m_status);
    }
    static void step(Kat500Model& k, const QByteArray& reply={}) {
        k.tick(); if(!reply.isEmpty()) { k.receiveBytes(reply); }
    }
    static void observe(Kat500Model& k, const QByteArray& query, const QByteArray& reply) { k.query(query); step(k,reply); }
    static void frame(Kat500Model& k, const QByteArray& reply) { k.receiveBytes(reply); }
    static void advance(Kat500Model& k, qint64 now) { k.m_now=[now] { return now; }; k.m_lastIo=now; k.m_lastReply=now; }
    static void drop(Kat500Model& k) { k.transportClosed(); }
    static void stale(Kat500Model& k) {
        k.m_now=[] { return qint64(6000); };
        emit k.availabilityChanged();
    }
    static void reopen(Kat500Model& k) {
        k.transportOpened(); k.m_timer.stop();
    }
};
}
namespace {
int failures=0;
void check(bool ok, const char* what) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",what); failures+=!ok; }
class Backend final : public IRadioBackend {
public:
    QStringList wire;
    bool connected=false;
    RadioCapabilities capabilities() const override { RadioCapabilities c; c.canTransmit=true; c.hasTuner=true; return c; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int,double) override {}
    void setSliceMode(int,const QString&) override {}
    void setSliceFilter(int,int,int) override {}
    void setSliceAgc(int,const QString&,int) override {}
    void setPanCenter(const QString&,double,PanCenterIntent) override {}
    void setKeying(bool,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override {}
    void setAtu(bool on,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override { wire << (on?"atu:on":"atu:off"); }
    void setTune(bool on,int,const TxCoordinator::Operation&,const TxCoordinator::Completion&) override { wire << (on?"tune:on":"tune:off"); }
    void invokeExtension(const QString&,const QString&,quint64,const QVariant&) override {}
};
struct Fixture {
    RadioModel radio;
    Kat500Model kat;
    QList<QByteArray> serial;
    Backend* backend;
    std::shared_ptr<TxController> input;
    std::unique_ptr<Kat500TxController> routing;
    Fixture() {
        auto b=std::make_unique<Backend>(); backend=b.get(); radio.setBackendForTest(std::move(b),"test");
        check(radio.automationApplySliceFixture(0,"A"),"socket-free TX slice fixture");
        SliceDelta slice; slice.txSlice=true; slice.mode="USB"; slice.panId="0x40000000"; slice.frequency=14.1;
        radio.slice(0)->applyChanges(slice);
        backend->connected=true;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        radio.transmitModel().setHasTuner(true);
        TransmitDelta d; d.transmitFreq=14.1; d.tunePower=3; d.atuEnabled=true; d.atuStatusRaw="TUNE_SUCCESSFUL";
        radio.transmitModel().applyChanges(d);
        Kat500ModelTest::ready(kat,serial);
        routing=std::make_unique<Kat500TxController>(&radio,&kat);
        input=std::make_shared<TxController>(&radio);
    }
    void bypass() { TransmitDelta d; d.atuStatusRaw="TUNE_BYPASS"; radio.transmitModel().applyChanges(d); }
    bool start() { return routing->start(input->capture(TxController::Activity::Tune)); }
    void readResult() {
        for (const QByteArray& reply : {QByteArray("MDA;"), QByteArray("AN1;"), QByteArray("BYPN;"),
             QByteArray("SIDET;"), QByteArray("C02;"), QByteArray("L03;"), QByteArray("VSWR1.33;"), QByteArray("FLT0;"),
             QByteArray("TP0;"), QByteArray("VFWD0;")}) {
            Kat500ModelTest::step(kat, reply);
        }
    }
    void acknowledgeSearch() {
        Kat500ModelTest::step(kat,"F 14100;");
        Kat500ModelTest::step(kat,"TP1;");
    }
};
void routingAndCycle() {
    Fixture f;
    check(f.kat.useTxControls() && f.routing->selected(),"default-on routing selects confirmed Auto");
    check(f.backend->wire==QStringList{"atu:off"},"selecting external ATU requests internal bypass without RF");
    check(f.start(),"explicit click captures a tune request");
    Kat500ModelTest::step(f.kat);
    check(f.serial.isEmpty() && !f.backend->wire.contains("tune:on"),"wait for confirmed internal bypass before tuner or RF start");
    f.bypass(); f.acknowledgeSearch();
    check(f.serial.contains("F 14100;F;") && f.serial.contains("T;TP;"),"tune targets current TX frequency then full search");
    check(f.backend->wire.count("tune:on")==1 && !f.backend->wire.contains("atu:on"),"one captured radio carrier; internal ATU never starts");
    check(f.kat.status().mode==Kat500::Mode::Auto,"fresh tune preserves Auto mode");
    QTest::qWait(220);
    check(f.routing->busy() && f.radio.transmitModel().isTuning()
          && !f.backend->wire.contains("tune:off"),
          "watchdog retains the carrier when the backend provides no Tune readback");
    Kat500ModelTest::frame(f.kat,"FT;");
    check(f.backend->wire.count("tune:off")==1,"completion stops its carrier immediately");
    f.readResult();
    check(!f.routing->busy() && f.routing->matched(), "post-tune relay readback confirms the new match");
    check(f.radio.transmitModel().tunePower()==3,"tune does not raise operator drive power");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Bypass);
    check(!f.routing->selected(),"explicit Bypass restores internal route");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Manual);
    check(f.routing->selected(),"Manual selects external route");
    f.kat.setUseTxControls(false);
    check(!f.routing->selected(),"preference off restores internal route");
}
void cancellationAndFences() {
    {
        Fixture f;
        Kat500ModelTest::stale(f.kat);
        check(f.routing->selected() && !f.routing->unavailableReason().isEmpty(),
            "stale readback on an open connection blocks actions without changing tuner route");
        Kat500ModelTest::drop(f.kat);
        check(!f.routing->selected(),"closing a stale connection restores the radio route");
        Kat500ModelTest::reopen(f.kat);
        check(!f.routing->selected(),"reconnect waits for fresh tuner mode before selecting KAT500");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch(); f.routing->cancel(); f.routing->cancel();
        Kat500ModelTest::step(f.kat);
        Kat500ModelTest::advance(f.kat,2100); Kat500ModelTest::step(f.kat);
        Kat500ModelTest::advance(f.kat,4200); Kat500ModelTest::step(f.kat);
        check(f.serial.count("CT;TP;")==1 && !f.routing->busy(), "missing cancellation acknowledgement is terminal without CT retries");
    }
    {
        Fixture f; f.bypass(); f.start();
        const auto competing = f.input->capture(TxController::Activity::Mox);
        check(competing.start(), "offline competing MOX starts");
        f.acknowledgeSearch();
        check(!f.serial.contains("T;TP;") && !f.backend->wire.contains("tune:on"), "competing transmission cancels tune preflight");
        competing.stop();
    }
    {
        Fixture f; f.bypass(); f.start();
        Kat500ModelTest::step(f.kat,"F 14100;");
        Kat500ModelTest::step(f.kat);
        check(f.serial.contains("T;TP;"), "pending tuner request was dispatched");
        f.routing.reset();
        Kat500ModelTest::advance(f.kat,2100);
        Kat500ModelTest::step(f.kat);
        check(f.serial.contains("CT;TP;") && !f.backend->wire.contains("tune:on"), "controller destruction retains tuner cancellation after late replies drain");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::frame(f.kat,"FT;");
        f.readResult();
        check(f.routing->matched(), "confirmed completion exposes successful match");
        Kat500ModelTest::observe(f.kat,"AN;","AN2;");
        check(!f.routing->matched(), "antenna change clears previous match success");
    }
    {
        Fixture f; f.start(); f.routing->cancel(); f.bypass();
        Kat500ModelTest::step(f.kat,"TP1;");
        check(!f.backend->wire.contains("tune:on") && f.serial.isEmpty(),"cancel before bypass readback prevents all delayed tune work");
    }
    {
        Fixture f; f.bypass(); f.start(); f.input->invalidate();
        f.acknowledgeSearch();
        check(!f.backend->wire.contains("tune:on") && !f.serial.contains("T;TP;"),"revoked input prevents queued tuner search and carrier");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::drop(f.kat);
        check(f.backend->wire.contains("tune:off") && !f.routing->busy(),"disconnect stops carrier");
        check(!f.routing->selected(),"closed tuner connection releases the external route");
        check(!f.backend->wire.contains("atu:on"),"disconnect never starts the internal ATU");
    }
    {
        Fixture f; f.bypass(); f.start(); f.acknowledgeSearch();
        Kat500ModelTest::mode(f.kat,Kat500::Mode::Bypass);
        check(f.backend->wire.contains("tune:off") && !f.routing->selected(),"mode change cancels owned carrier before returning internal route");
    }
    {
        Fixture f; f.bypass(); f.start();
        auto* deadline=f.routing->findChild<QTimer*>("katTxDeadline");
        check(deadline,"bounded tune deadline exists");
        if(deadline) { QMetaObject::invokeMethod(deadline,"timeout",Qt::DirectConnection); }
        f.acknowledgeSearch();
        check(!f.backend->wire.contains("tune:on"),"timeout cannot be revived by a late tuner reply");
    }
}
QPushButton* button(TxApplet& a,const QString& name) {
    for(auto* b:a.findChildren<QPushButton*>()) { if(b->accessibleName()==name) { return b; } }
    return nullptr;
}
void saving() {
    Fixture f;
    check(f.routing->saveMatch(), "SAVE accepts an explicit no-RF request");
    for (const QByteArray& reply : {QByteArray("KAT500;"), QByteArray("RV02.16;"), QByteArray("SN00222;"),
         QByteArray("PS1;"), QByteArray("TP0;"), QByteArray("VFWD0;"), QByteArray("AN1;"),
         QByteArray("BYPN;"), QByteArray("SIDET;"), QByteArray("C0;"), QByteArray("L3;")}) {
        Kat500ModelTest::step(f.kat, reply);
    }
    Kat500ModelTest::step(f.kat);
    check(f.serial.contains("SM 14100;DM14100;"), "SAVE writes and verifies the current TX frequency bin");
    Kat500ModelTest::frame(f.kat,"DM 14100-14119;AN1;SIDET;C00;L03;VSWRB1.00;5 UNUSED;");
    check(!f.routing->busy() && f.routing->message().contains("saved and verified"), "SAVE completes only after matching memory readback");
    check(!f.backend->wire.contains("tune:on") && !f.serial.contains("T;TP;"), "SAVE never requests a search or carrier");
}
void nativeSweepRouteFence() {
    for (const bool paused : {false, true}) {
        Fixture f;
        Kat500ModelTest::mode(f.kat, Kat500::Mode::Bypass);
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        check(!dialog.usesKat500(), "Bypass captures the native pre-tune route");
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        if (paused) { TxAppletPowerReconciliationTestAccess::pause(dialog); }
        Kat500ModelTest::mode(f.kat, Kat500::Mode::Auto);
        Kat500ModelTest::mode(f.kat, Kat500::Mode::Bypass);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        check(!TxAppletPowerReconciliationTestAccess::active(dialog)
            && !f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on"),
            "route change invalidates native sweep even if Bypass returns before settle");
    }
}
void externalSweep() {
    {
        Fixture f;
        BandPlanManager plan;
        plan.setSegmentsForTest({{1.8,1.83,"","",{}}, {14.0,14.06,"","",{}},
            {28.0,28.3,"","",{}}, {50.0,50.4,"","",{}}, {5.3306,5.3334,"","",{}}});
        AtuPreTuneDialog dialog(&f.radio, &plan, nullptr, f.routing.get());
        const auto centers = [&dialog](const QString& band) { return TxAppletPowerReconciliationTestAccess::centers(dialog, band); };
        check(centers("160m") == QVector<double>{1.805,1.815,1.825}
            && centers("20m") == QVector<double>{14.010,14.030,14.050}
            && centers("10m") == QVector<double>{28.050,28.150,28.250}
            && centers("6m") == QVector<double>{50.100,50.300}, "sweep points cover KAT500 bins with documented band-specific widths");
        check(centers("60m") == QVector<double>{5.332}, "narrow legal channel uses its midpoint without crossing a gap");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::twoPoints(dialog);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        Kat500ModelTest::frame(f.kat,"FT;"); f.readResult(); QCoreApplication::processEvents();
        TransmitDelta delta; delta.transmitFreq=14.12; f.radio.transmitModel().applyChanges(delta);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat,"F 14120;"); Kat500ModelTest::step(f.kat,"TP1;");
        check(f.backend->wire.count("tune:on") == 2,
            "two-point sweep refreshes idle/RF state before starting the next frequency");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::pause(dialog);
        Kat500ModelTest::drop(f.kat); QCoreApplication::processEvents();
        check(!TxAppletPowerReconciliationTestAccess::active(dialog),
            "disconnect invalidates a sweep paused between points");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        auto other = std::make_shared<TxController>(&f.radio);
        check(f.routing->start(other->capture(TxController::Activity::Tune)), "independent operator tune starts");
        f.acknowledgeSearch();
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat);
        check(f.routing->busy() && !f.serial.contains("CT;TP;") && !f.backend->wire.contains("tune:off"),
            "refused sweep point does not cancel a separate operator tune");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        check(f.serial.contains("T;TP;") && f.backend->wire.contains("tune:on")
            && !f.backend->wire.contains("atu:on"), "external sweep drives KAT500 search with radio carrier only");
        Kat500ModelTest::frame(f.kat,"FT;");
        QCoreApplication::processEvents();
        check(TxAppletPowerReconciliationTestAccess::active(dialog), "sweep waits for match readback after search ends");
        f.readResult(); QCoreApplication::processEvents();
        check(!TxAppletPowerReconciliationTestAccess::active(dialog)
            && TxAppletPowerReconciliationTestAccess::successes(dialog) == 1,
            "KAT500 result completes the sweep without internal ATU status");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        f.acknowledgeSearch();
        TxAppletPowerReconciliationTestAccess::cancel(dialog);
        Kat500ModelTest::step(f.kat,"TP0;");
        check(f.backend->wire.contains("tune:off") && f.serial.contains("CT;TP;"),
            "aborting sweep stops its carrier and cancels the external search");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        f.routing.reset();
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        check(!f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on"),
            "destroyed external route cannot fall back to internal ATU");
    }
    {
        Fixture f; f.bypass();
        AtuPreTuneDialog dialog(&f.radio, nullptr, nullptr, f.routing.get());
        TxAppletPowerReconciliationTestAccess::prepare(dialog, f.input);
        TransmitDelta delta; delta.transmitFreq=7.1; f.radio.transmitModel().applyChanges(delta);
        TxAppletPowerReconciliationTestAccess::settle(dialog);
        Kat500ModelTest::step(f.kat);
        check(f.serial.isEmpty() && !f.backend->wire.contains("tune:on"),
            "sweep rejects a TX frequency that differs from the planned point");
    }
}
void contextMenu() {
    Fixture f; f.routing.reset();
    TxApplet applet;
    applet.setRadioModel(&f.radio);
    applet.setTransmitModel(&f.radio.transmitModel());
    applet.setKat500Model(&f.kat);
    QMenu menu;
    applet.buildAtuContextMenu(menu);
    const auto actions = menu.actions();
    check(actions.size() == 2 && actions[0]->text().startsWith("Pre-tune bands")
        && actions[1]->text().startsWith("Clear all KAT500 memories"),
        "KAT500 context menu offers its sweep and all-memory erase only");
    if (actions.size() == 2) {
        check(actions[1]->isEnabled(), "ready KAT500 exposes confirmed erase action");
        QTimer::singleShot(0, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                    check(box->text().contains("KAT500"), "erase confirmation identifies external tuner");
                    box->reject();
                }
            }
        });
        actions[1]->trigger();
        Kat500ModelTest::step(f.kat);
        check(f.serial.isEmpty(), "canceling erase confirmation sends no tuner command");
        QTimer::singleShot(0, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                    for (auto* b : box->buttons()) {
                        if (box->buttonRole(b) == QMessageBox::DestructiveRole) { b->click(); return; }
                    }
                }
            }
        });
        actions[1]->trigger();
        for (int band = 0; band < 11; ++band) {
            for (const QByteArray& reply : {QByteArray("KAT500;"), QByteArray("RV02.16;"),
                 QByteArray("SN00222;"), QByteArray("PS1;"), QByteArray("TP0;"),
                 QByteArray("VFWD0;"), QByteArray("RV02.16;")}) { Kat500ModelTest::step(f.kat, reply); }
            check(f.serial.count("EM" + QByteArray::number(band).rightJustified(2, '0') + "0;RV;") == 1,
                "confirmed clear targets all antennas of each KAT500 band once");
        }
        check(!f.kat.operationBusy() && !f.backend->wire.contains("tune:on"),
            "all-memory clear completes without any radio carrier");
    }
    Kat500ModelTest::drop(f.kat);
    QMenu disconnected;
    applet.buildAtuContextMenu(disconnected);
    check(disconnected.actions().size() == 2
        && !disconnected.actions()[0]->text().contains("KAT500")
        && !disconnected.actions()[1]->text().contains("KAT500"),
        "disconnected KAT500 restores the radio context menu");
}
void presentation() {
    Fixture f; f.routing.reset();
    TxApplet applet; applet.setRadioModel(&f.radio); applet.setTransmitModel(&f.radio.transmitModel()); applet.setKat500Model(&f.kat);
    auto* atu=button(applet,"ATU tune"); auto* save=button(applet,"ATU memories");
    check(atu && save,"existing TX Controls retain accessible identities"); if(!atu || !save) { return; }
    check(atu->contextMenuPolicy() == Qt::CustomContextMenu, "external AUTO retains right-click menu access");
    check(atu->text()=="AUTO" && save->text()=="SAVE" && !save->isCheckable(),"Auto uses AUTO and momentary SAVE");
    bool pill=false; for(auto* l:applet.findChildren<QLabel*>()) { pill|=l->text()=="KAT500"; }
    check(pill,"KAT500 occupies existing memory status slot");
    QLabel* katPill = nullptr;
    for (auto* label : applet.findChildren<QLabel*>()) {
        if (label->text() == "KAT500") { katPill = label; }
    }
    if (katPill) {
        const QString passStyle = katPill->styleSheet();
        Kat500ModelTest::observe(f.kat, "AMPI;", "AMPI1;");
        katPill->ensurePolished();
        check(katPill->toolTip().contains("KEY BLOCK")
                  && katPill->accessibleDescription().contains("radio watts")
                  && katPill->palette().color(QPalette::WindowText)
                      == ThemeManager::instance().color("color.accent.warning"),
              "confirmed key block turns the existing KAT500 pill amber with a radio-meter explanation");
        Kat500ModelTest::observe(f.kat, "AMPI;", "AMPI0;");
        check(katPill->styleSheet() == passStyle && !katPill->toolTip().contains("KEY BLOCK"),
              "confirmed key pass restores the normal KAT500 pill");
        f.serial.clear();
    }

    Kat500ModelTest::mode(f.kat,Kat500::Mode::Manual);
    check(atu->text()=="ATU" && save->text()=="SAVE","Manual shows ATU and SAVE");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Bypass);
    check(atu->text()=="ATU" && save->text()=="MEM" && save->isCheckable(),"Bypass restores existing ATU/MEM presentation");
    check(!atu->accessibleDescription().contains("KAT500"), "Bypass restores radio ATU accessible description");
    auto* tune=button(applet,"Tune");
    check(tune && atu->styleSheet()==tune->styleSheet(),"Bypass restores the original radio button style");
    Kat500ModelTest::mode(f.kat,Kat500::Mode::Auto);
    TransmitDelta power; power.rfPower=50; power.tunePower=7;
    f.radio.transmitModel().applyChanges(power);
    const auto hasLabel = [&applet](const QString& text) {
        for (auto* label : applet.findChildren<QLabel*>()) { if (label->text()==text) { return true; } }
        return false;
    };
    check(hasLabel("50%") && hasLabel("7%"),"selected KAT500 keeps percentage power readouts");
    const TxKeyingAction::Prepared pending = prepareTxKeyingAction(atu, f.input, "click", {});
    check(bool(pending), "external ATU click can be captured before disconnect");
    f.backend->wire.clear();
    f.kat.disconnectDevice();
    check(atu->text()=="ATU" && save->text()=="MEM" && save->isCheckable(),
        "disconnect restores ATU and checkable MEM immediately");
    pill=false;
    bool memory=false;
    for (auto* label : applet.findChildren<QLabel*>()) {
        pill |= label->text()=="KAT500";
        memory |= label->text()=="Mem";
    }
    check(!pill && memory,"disconnect replaces the KAT500 pill with radio memory status");
    check(tune && atu->styleSheet()==tune->styleSheet(),"disconnect restores the original radio button style");
    check(hasLabel("50") && hasLabel("7") && !hasLabel("50%") && !hasLabel("7%"),
        "disconnect restores plain radio power readouts without another meter update");
    if (pending) { pending(); }
    Kat500ModelTest::step(f.kat);
    check(!f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on")
        && f.serial.isEmpty(),"captured external click cannot key either tuner after disconnect");
    f.bypass();
    f.backend->wire.clear();
    const TxKeyingAction::Prepared fresh = prepareTxKeyingAction(atu, f.input, "click", {});
    if (fresh) { fresh(); }
    check(f.backend->wire.contains("atu:on") && !f.backend->wire.contains("tune:on")
        && f.serial.isEmpty(),"a new operator click after disconnect uses only the radio ATU");
}
}
int main(int argc,char** argv) {
    TestSettingsProfile profile("kat500-tx-controls"); if(!profile.isValid()) { return 1; }
    QApplication app(argc,argv);
    routingAndCycle(); cancellationAndFences(); saving(); presentation(); contextMenu(); nativeSweepRouteFence(); externalSweep();
    return failures ? 1:0;
}
