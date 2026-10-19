// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Ported from the N8EM Orion console (src/log/QsoContext.h) for the AetherSDR
// Orion fork (logbook/CW integration): namespace and include paths only.

#include <QObject>
#include <QString>

namespace AetherSDR::orion::logbook {

// The station being worked RIGHT NOW, shared by every window that has an
// opinion about it. Before this the Log QSO window and the CW window each
// kept their own idea of the DX: a call typed in one never reached the
// other, and the RST you just took down lived nowhere the CW macros could
// see it.
//
// One record, two rules: a setter that changes nothing emits nothing (so
// the log and the CW window can mirror each other without a feedback
// loop), and the window the operator is typing in is always the writer.
// Cleared when the QSO is logged or the form is wiped.
class QsoContext : public QObject {
    Q_OBJECT

public:
    explicit QsoContext(QObject* parent = nullptr) : QObject(parent) {}

    [[nodiscard]] const QString& call() const { return m_call; }
    [[nodiscard]] const QString& handle() const { return m_handle; }   // the on-air name
    [[nodiscard]] const QString& name() const { return m_name; }       // the callbook's
    [[nodiscard]] const QString& qth() const { return m_qth; }
    [[nodiscard]] const QString& grid() const { return m_grid; }
    [[nodiscard]] const QString& rstSent() const { return m_rstSent; }
    [[nodiscard]] const QString& rstRcvd() const { return m_rstRcvd; }

    void setCall(const QString& v) { set(m_call, v.trimmed().toUpper()); }
    void setHandle(const QString& v) { set(m_handle, v.trimmed()); }
    void setName(const QString& v) { set(m_name, v.trimmed()); }
    void setQth(const QString& v) { set(m_qth, v.trimmed()); }
    void setGrid(const QString& v) { set(m_grid, v.trimmed().toUpper()); }
    void setRstSent(const QString& v) { set(m_rstSent, v.trimmed()); }
    void setRstRcvd(const QString& v) { set(m_rstRcvd, v.trimmed()); }

    void clear()
    {
        if (m_call.isEmpty() && m_handle.isEmpty() && m_name.isEmpty() && m_qth.isEmpty()
            && m_grid.isEmpty() && m_rstSent.isEmpty() && m_rstRcvd.isEmpty()) {
            return;
        }
        m_call.clear();
        m_handle.clear();
        m_name.clear();
        m_qth.clear();
        m_grid.clear();
        m_rstSent.clear();
        m_rstRcvd.clear();
        emit changed();
    }

signals:
    void changed();

private:
    void set(QString& slot, const QString& v)
    {
        if (slot == v) {
            return;   // no echo, no loop
        }
        slot = v;
        emit changed();
    }

    QString m_call, m_handle, m_name, m_qth, m_grid, m_rstSent, m_rstRcvd;
};

}  // namespace AetherSDR::orion::logbook
