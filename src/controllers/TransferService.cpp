#include "controllers/TransferService.h"

#include <qbucket/s3/transfer/JsonFileResumeStore.h>

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace qbucket {

namespace {

QString defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/transfers");
}

} // namespace

TransferService::TransferService(QObject* parent)
    : TransferService(defaultDirectory(), parent)
{
}

TransferService::TransferService(const QString& directory, QObject* parent)
    : QObject(parent),
      m_dir(directory),
      m_store(std::make_shared<s3::transfer::JsonFileResumeStore>(QDir(directory).filePath(QStringLiteral("parts"))))
{
}

TransferService::~TransferService() = default;

QString TransferService::jobDirectory() const
{
    return QDir(m_dir).filePath(QStringLiteral("jobs"));
}

s3::transfer::AdaptiveConcurrency* TransferService::limiter(const QUuid& account)
{
    auto& slot = m_limiters[account];
    if (!slot) {
        s3::transfer::AdaptiveConcurrency::Options o;
        o.max = m_maxParts;
        o.initial = std::min(o.initial, o.max);
        slot = std::make_unique<s3::transfer::AdaptiveConcurrency>(o);
    }
    return slot.get();
}

void TransferService::setMaxParallelParts(int parts)
{
    m_maxParts = std::max(1, parts);
    for (auto& [id, l] : m_limiters)
        l->setMaximum(m_maxParts);
}

} // namespace qbucket
