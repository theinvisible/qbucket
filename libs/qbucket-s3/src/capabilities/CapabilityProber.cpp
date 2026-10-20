// S3Client's capability probes (PLAN.md §3.3): read-only GET requests on
// bucket and object subresources, at most four at a time, interpreted by
// ProbeInterpreter. Never writes anything.

#include "capabilities/ProbeInterpreter.h"
#include "client/S3Client.h"
#include "xml/XmlParsers.h"

#include <qbucket/s3/Logging.h>

#include <QTimer>

namespace qbucket::s3 {
namespace {

constexpr int kParallelProbes = 4;

bool stopsProbing(const Error& e)
{
    switch (e.category) {
    case ErrorCategory::NotFound:          // no such bucket: nothing to learn
    case ErrorCategory::Authentication:
    case ErrorCategory::Credentials:
    case ErrorCategory::Cancelled:
    case ErrorCategory::ClockSkew:
        return true;
    default:
        return false;
    }
}

} // namespace

struct S3Client::ProbeRun {
    std::shared_ptr<Op> op;
    QString bucket;
    QString objectKey;
    QList<ProbeSpec> queue;
    CapabilitySet result;
    Done<CapabilitySet> done;
    int inFlight = 0;
    bool finished = false;
};

quint64 S3Client::probeServer(Done<CapabilitySet> done)
{
    RequestSpec spec;
    spec.operation = Operation::ListBuckets;
    const auto op = newOp();
    send(op, std::move(spec), [this, op, done = std::move(done)](Result<RawResponse> r) {
        endOp(op);
        if (!r && stopsProbing(r.error())) {
            done(r.error());
            return;
        }
        CapabilitySet set = config().profile->capabilities();
        set.scope = CapabilityScope::Server;
        set.probedAt = QDateTime::currentDateTimeUtc();
        if (r)
            set.fingerprint = fingerprintOf(r.value());
        done(std::move(set));
    });
    return op->id;
}

quint64 S3Client::probeBucket(const QString& bucket, const QString& objectKey, Done<CapabilitySet> done)
{
    if (bucket.isEmpty())
        return failLater(std::move(done), Error::make(ErrorCategory::InvalidRequest,
                                                      QStringLiteral("bucket is missing")));
    auto run = std::make_shared<ProbeRun>();
    run->op = newOp();
    run->bucket = bucket;
    run->objectKey = objectKey;
    run->done = std::move(done);
    run->result.scope = CapabilityScope::Bucket;
    run->result.bucket = bucket;
    run->result.probedAt = QDateTime::currentDateTimeUtc();

    // One small listing first: fingerprint, a key for object-level probes,
    // and an early exit when the bucket isn't reachable at all.
    RequestSpec list;
    list.operation = Operation::ListObjectsV2;
    list.bucket = bucket;
    list.query = {{"list-type", "2", false}, {"max-keys", "1", false}, {"encoding-type", "url", false}};
    send(run->op, std::move(list), [this, run](Result<RawResponse> r) {
        if (!r && stopsProbing(r.error())) {
            run->finished = true;
            endOp(run->op);
            run->done(r.error());
            return;
        }
        if (r) {
            run->result.fingerprint = fingerprintOf(r.value());
            const Result<ListPage> page =
                xml::parseListObjectsV2(r->body, config().profile->listingDecodesPlusAsSpace());
            if (run->objectKey.isEmpty() && page && !page->objects.isEmpty())
                run->objectKey = page->objects.first().key;
        }
        run->queue = bucketProbes();
        if (!run->objectKey.isEmpty()) {
            run->queue += objectProbes();
        } else {
            // Empty bucket: object-level features stay unverified.
            for (const ProbeSpec& p : objectProbes()) {
                CapabilityEntry e;
                e.source = CapabilitySource::Probed;
                e.checkedAt = run->result.probedAt;
                e.unknownAttempts = 1;
                e.s3Code = QStringLiteral("NoObjectToProbe");
                run->result.set(p.feature, e);
            }
        }
        runProbes(run);
    });
    return run->op->id;
}

void S3Client::runProbes(const std::shared_ptr<ProbeRun>& run)
{
    while (run->inFlight < kParallelProbes && !run->queue.isEmpty()) {
        const ProbeSpec probe = run->queue.takeFirst();
        RequestSpec spec;
        spec.operation = probe.objectLevel ? Operation::GetObjectSubresource
                                           : Operation::GetBucketSubresource;
        spec.bucket = run->bucket;
        if (probe.objectLevel)
            spec.key = run->objectKey;
        spec.query = probe.query;
        ++run->inFlight;
        send(run->op, std::move(spec), [this, run, probe](const Result<RawResponse>& r) {
            --run->inFlight;
            CapabilityEntry e =
                interpretProbe(probe, r, QDateTime::currentDateTimeUtc(), &run->result.bucketState);
            config().profile->reviewProbe(probe.feature, e);
            run->result.set(probe.feature, e);
            qCDebug(lcCapabilities).noquote() << run->bucket << featureId(probe.feature) << stateName(e.state)
                                              << e.httpStatus << e.s3Code;
            runProbes(run);
        });
    }
    if (run->inFlight == 0 && run->queue.isEmpty() && !run->finished) {
        run->finished = true;
        endOp(run->op);
        if (run->op->cancelled) {
            run->done(Error::make(ErrorCategory::Cancelled, QStringLiteral("probe cancelled")));
            return;
        }
        run->done(std::move(run->result));
    }
}

} // namespace qbucket::s3
