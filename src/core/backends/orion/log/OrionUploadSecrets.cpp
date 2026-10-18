#include "core/backends/orion/log/OrionUploadSecrets.h"

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR::orion::logbook {

namespace {
constexpr const char* kKeychainService = "AetherSDR";

QString keychainKey(const QString& key)
{
    QString k = key;
    k.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("orion_upload_") + k;
}
}  // namespace

const QStringList& OrionUploadSecrets::keys()
{
    static const QStringList k = {
        QStringLiteral("lotw/password"),     // LoTW website login (QSL download)
        QStringLiteral("lotw/tqslPassword"), // the station certificate's key password
        QStringLiteral("eqsl/password"),
        QStringLiteral("qrz/key"),           // QRZ logbook API key
        QStringLiteral("club/password"),
        QStringLiteral("club/apikey"),       // ClubLog application API key
        QStringLiteral("hrdlog/code"),       // HRDLOG upload code
    };
    return k;
}

OrionUploadSecrets::OrionUploadSecrets(bool memoryOnly, QObject* parent)
    : QObject(parent)
    , m_memoryOnly(memoryOnly)
{
}

bool OrionUploadSecrets::persistent() const
{
#ifdef HAVE_KEYCHAIN
    return !m_memoryOnly;
#else
    return false;
#endif
}

void OrionUploadSecrets::load()
{
#ifdef HAVE_KEYCHAIN
    if (!m_memoryOnly) {
        m_pending = int(keys().size());
        for (const QString& key : keys()) {
            auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
            job->setAutoDelete(true);
            job->setKey(keychainKey(key));
            connect(job, &QKeychain::Job::finished, this, [this, key](QKeychain::Job* j) {
                auto* read = static_cast<QKeychain::ReadPasswordJob*>(j);
                if (j->error() == QKeychain::NoError && !read->textData().isEmpty()) {
                    m_values.insert(key, read->textData());
                }
                if (--m_pending == 0) {
                    emit loaded();
                }
            });
            job->start();
        }
        return;
    }
#endif
    m_pending = 0;
    emit loaded();
}

void OrionUploadSecrets::setValue(const QString& key, const QString& value)
{
    const QString v = value.trimmed();
    if (v.isEmpty()) {
        m_values.remove(key);
    } else {
        m_values.insert(key, v);
    }
#ifdef HAVE_KEYCHAIN
    if (m_memoryOnly) {
        return;
    }
    if (v.isEmpty()) {
        auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(keychainKey(key));
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(keychainKey(key));
    job->setTextData(v);
    connect(job, &QKeychain::Job::finished, this, [this, key](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError) {
            emit saveFailed(key, j->errorString());
        }
    });
    job->start();
#endif
}

}  // namespace AetherSDR::orion::logbook
