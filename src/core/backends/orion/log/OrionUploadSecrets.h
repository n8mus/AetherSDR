#pragma once

// The online-log uploads' secrets (passwords, API keys, upload codes), kept
// in the system keychain, AetherSDR's only persistent credential store (RFC
// #4603), never in the settings file. Loaded into memory at startup; the
// uploader reads them from here. Logins, callsigns and paths are ordinary
// settings (OrionUp/...).
//
// Keys are "<service>/<field>", e.g. "eqsl/password"; in the keychain they
// live under the "AetherSDR" service as "orion_upload_<service>_<field>".

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

namespace AetherSDR::orion::logbook {

class OrionUploadSecrets : public QObject {
    Q_OBJECT

public:
    // Every secret the uploader may ask for.
    static const QStringList& keys();

    // memoryOnly: never touch the keychain (tests).
    explicit OrionUploadSecrets(bool memoryOnly = false, QObject* parent = nullptr);

    void load();                                  // async; loaded() when done
    [[nodiscard]] QString value(const QString& key) const { return m_values.value(key); }
    void setValue(const QString& key, const QString& value);   // writes through
    [[nodiscard]] bool isLoaded() const { return m_pending == 0; }
    [[nodiscard]] bool persistent() const;        // false: this session only

signals:
    void loaded();
    void saveFailed(const QString& key, const QString& reason);

private:
    bool m_memoryOnly;
    QHash<QString, QString> m_values;
    int m_pending = 0;
};

}  // namespace AetherSDR::orion::logbook
