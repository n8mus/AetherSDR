#include "TestSettingsProfile.h"
#include "gui/TxApplet.h"
#include "models/Kxpa100Model.h"
#include "models/Kxpa100TxController.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QTest>
#include <cstdio>
using namespace AetherSDR;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (false)
namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void identity(RadioModel& r, const QString& serial = "HL2-TEST") { r.m_lastInfo.serial=serial; emit r.infoChanged(); }
    static void family(RadioModel& r, const QString& family) { r.m_family=family; }
};
class Kxpa100ModelTest {
public:
    static void ready(Kxpa100Model& k, QList<QByteArray>& wire) {
        k.m_timer.stop(); k.m_now=[] { return qint64(100); }; k.m_lastReply=100; k.m_nextPoll=100000;
        k.m_write=[&wire](const QByteArray& bytes) { wire.append(bytes); return true; };
        k.m_connected=k.m_detected=true;
        auto& s=k.m_status; s.identity="KXPA100"; s.firmware="01.39"; s.serial="00031";
        s.tuning=false; s.inputWatts=0; s.forwardWatts=0; s.tunerPresent=true;
        s.mode=Kxpa100::Mode::Auto; s.antenna=2; s.operate=true; s.faultCode='N';
        for (const QString& key : {"^I","^RV","^SN","^TP","^PI","^PF","^TU","^MD","^AN","^OP","^FL"}) { k.m_received[key]=100; }
    }
    static void frame(Kxpa100Model& k,const QByteArray& reply) { k.receiveBytes(reply); }
    static void observe(Kxpa100Model& k,const QByteArray& query,const QByteArray& reply) { k.query(query); step(k,reply); }
    static void step(Kxpa100Model& k,const QByteArray& reply) { k.tick(); k.receiveBytes(reply); }
    static void sendNext(Kxpa100Model& k) { k.tick(); }
    static void preflight(Kxpa100Model& k) {
        for (const QByteArray& s : {QByteArray("^IKXPA100;"),QByteArray("^RV01.39;"),QByteArray("^SN00031;"),QByteArray("^TP0;"),QByteArray("^PI0000;"),QByteArray("^PF0000;")}) { step(k,s); }
    }
    static void stale(Kxpa100Model& k) { k.m_now=[] { return qint64(6100); }; emit k.availabilityChanged(); }
    static void drop(Kxpa100Model& k) { k.transportClosed(); }
    static void match(Kxpa100Model& k) {
        for (const QByteArray& s : {QByteArray("^TP0;"),QByteArray("^MDA;"),QByteArray("^AN2;"),QByteArray("^BYN;"),QByteArray("^SIT;"),QByteArray("^CR00;"),QByteArray("^SW010;"),QByteArray("^SB010;"),QByteArray("^LR00;")}) { step(k,s); }
    }
};
}
class Backend final : public IRadioBackend {
public:
    QStringList wire;
    bool connected = false;
    RadioCapabilities capabilities() const override { RadioCapabilities c; c.canTransmit=true; c.hasTuner=false; return c; }
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
int main(int argc,char** argv) {
    TestSettingsProfile profile("kxpa100_tx_controls");
    QApplication app(argc,argv);
    AppSettings::instance().load();
    RadioModel radio; auto backend=std::make_unique<Backend>(); auto* b=backend.get();
    radio.setBackendForTest(std::move(backend),"test");
    CHECK(radio.automationApplySliceFixture(0,"A"));
    b->connected = true;
    SliceDelta slice; slice.txSlice=true; slice.mode="USB"; slice.frequency=14.1; slice.panId="0x40000000"; radio.slice(0)->applyChanges(slice);
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    radio.transmitModel().setHasTuner(false);
    // Host-modulated radios expose the TX slice without Flex's transmit-freq status.
    TransmitDelta delta; delta.transmitFreq=0; delta.tunePower=1; radio.transmitModel().applyChanges(delta);
    Kxpa100Model k; QList<QByteArray> wire; Kxpa100ModelTest::ready(k,wire);
    Kxpa100TxController routing(&radio,&k);
    CHECK(!routing.selected() && b->wire.isEmpty() && wire.isEmpty());
    // A fixture supplies the identity through the same RadioModel status path as discovery.
    TxOperationIntegrationTestAccess::identity(radio);
    CHECK(Kxpa100TxController::associate(&radio,&k));
    CHECK(routing.selected());
    // Discovery can announce identity before the selected backend family changes.
    TxOperationIntegrationTestAccess::family(radio,"previous-family");
    TxOperationIntegrationTestAccess::identity(radio);
    CHECK(!routing.associated());
    TxOperationIntegrationTestAccess::family(radio,"test");
    emit radio.connectionStateChanged(true);
    CHECK(routing.associated());
    CHECK(wire.isEmpty());
    CHECK(b->wire.isEmpty());
    {
        TxApplet applet; applet.setRadioModel(&radio); applet.setTransmitModel(&radio.transmitModel()); applet.setKxpa100Model(&k);
        QPushButton* atu=nullptr; QPushButton* save=nullptr;
        for (auto* button : applet.findChildren<QPushButton*>()) {
            if (button->accessibleName()=="ATU tune") { atu=button; }
            if (button->accessibleName()=="ATU memories") { save=button; }
        }
        CHECK(atu && save && atu->text()=="AUTO" && save->text()=="SAVE" && !save->isCheckable());
        Kxpa100ModelTest::observe(k,"^MD;","^MDB;");
        CHECK(!routing.selected() && atu->text()=="ATU" && save->text()=="MEM" && !atu->isEnabled());
        Kxpa100ModelTest::observe(k,"^MD;","^MDM;");
        CHECK(routing.selected() && atu->text()=="ATU" && save->text()=="SAVE");
        TxOperationIntegrationTestAccess::identity(radio,"OTHER-RADIO");
        CHECK(!routing.selected() && !applet.kxpa100Associated() && save->text()=="MEM");
        TxOperationIntegrationTestAccess::identity(radio);
        CHECK(routing.selected() && applet.kxpa100Associated());
        k.setUseTxControls(false); CHECK(!routing.selected() && save->text()=="MEM");
        k.setUseTxControls(true); CHECK(routing.selected());
        Kxpa100ModelTest::observe(k,"^MD;","^MDA;");
        CHECK(b->wire.isEmpty());
    }
    auto input=std::make_shared<TxController>(&radio);
    CHECK(routing.start(input->capture(TxController::Activity::Tune)));
    Kxpa100ModelTest::preflight(k);
    Kxpa100ModelTest::step(k,"^BN05;");
    Kxpa100ModelTest::match(k);
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::step(k,"^TP1;");
    CHECK(wire.contains("^FT;^TP;") && b->wire.count("tune:on")==1);
    CHECK(k.status().mode==Kxpa100::Mode::Auto && radio.transmitModel().tunePower()==1);
    Kxpa100ModelTest::frame(k,"^FE;");
    CHECK(b->wire.count("tune:off")==1);
    Kxpa100ModelTest::match(k);
    CHECK(routing.matched() && !routing.busy());
    CHECK(routing.saveMatch());
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::match(k);
    Kxpa100ModelTest::step(k,"^DM F14100 BN05 BIN040 FR14100-14119\r\nAN1 EMPTY\r\nAN2 BYN SIT CR00 LR00;");
    CHECK(wire.contains("^SM14100;^DM14100;") && !routing.busy() && b->wire.count("tune:on")==1);
    CHECK(routing.start(input->capture(TxController::Activity::Tune)));
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::step(k,"^BN05;"); Kxpa100ModelTest::match(k);
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::step(k,"^TP1;");
    CHECK(b->wire.count("tune:on")==2);
    Kxpa100ModelTest::stale(k);
    CHECK(!routing.selected() && b->wire.contains("tune:off"));
    Kxpa100ModelTest::drop(k);
    CHECK(!routing.selected() && !b->wire.contains("atu:on"));
    CHECK(Kxpa100TxController::dissociate(&radio,&k));
    CHECK(!routing.associated());
    Kxpa100ModelTest::ready(k,wire);
    CHECK(Kxpa100TxController::associate(&radio,&k));
    const int carriersBeforeAntennaChange=b->wire.count("tune:on");
    CHECK(routing.start(input->capture(TxController::Activity::Tune)));
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::step(k,"^BN05;");
    Kxpa100ModelTest::step(k,"^TP0;"); Kxpa100ModelTest::step(k,"^MDA;");
    Kxpa100ModelTest::step(k,"^AN1;");
    CHECK(!routing.busy() && b->wire.count("tune:on")==carriersBeforeAntennaChange);
    Kxpa100ModelTest::ready(k,wire);
    CHECK(routing.saveMatch());
    Kxpa100ModelTest::preflight(k);
    Kxpa100ModelTest::match(k);
    const int savesBeforeQsy=wire.count("^SM14100;^DM14100;");
    SliceDelta qsy; qsy.frequency=14.2; radio.slice(0)->applyChanges(qsy);
    Kxpa100ModelTest::step(k,"^DM F14100 BN05 BIN040 FR14100-14119\r\nAN1 EMPTY\r\nAN2 BYN SIT CR00 LR00;");
    CHECK(wire.count("^SM14100;^DM14100;")==savesBeforeQsy);
    CHECK(!routing.busy());
    qsy.frequency=14.1; radio.slice(0)->applyChanges(qsy);
    const int carriersBeforePendingCancel=b->wire.count("tune:on");
    CHECK(routing.start(input->capture(TxController::Activity::Tune)));
    Kxpa100ModelTest::preflight(k); Kxpa100ModelTest::step(k,"^BN05;"); Kxpa100ModelTest::match(k);
    Kxpa100ModelTest::preflight(k);
    Kxpa100ModelTest::sendNext(k);
    CHECK(wire.last()=="^FT;^TP;");
    routing.cancel();
    CHECK(k.isConnected() && routing.busy());
    Kxpa100ModelTest::frame(k,"^TP1;");
    Kxpa100ModelTest::step(k,"^TP0;");
    CHECK(wire.last()=="^FE;^TP;" && k.status().tuning==false && !routing.busy());
    CHECK(b->wire.count("tune:on")==carriersBeforePendingCancel);
    return 0;
}
