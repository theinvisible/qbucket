#pragma once

#include <qbucket/s3/Export.h>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace qbucket::s3 {

enum class SlashMode : quint8 { Encode, Keep };

// S3/SigV4 URI encoding: every byte except the unreserved set
// A-Z a-z 0-9 - . _ ~ becomes %XX (upper-case hex). With SlashMode::Keep, '/'
// stays literal (object keys in paths). Input is raw bytes — UTF-8 for keys;
// no Unicode normalisation, no path normalisation, never applied twice.
QBUCKET_S3_EXPORT QByteArray uriEncode(QByteArrayView bytes, SlashMode slash);

inline QByteArray uriEncodeText(const QString& text, SlashMode slash)
{
    return uriEncode(QByteArrayView(text.toUtf8()), slash);
}

// Inverse for listings returned with encoding-type=url. Decodes %XX only —
// '+' stays '+' unless plusIsSpace (some servers form-encode spaces).
// Returns false on a malformed escape.
QBUCKET_S3_EXPORT bool uriDecode(QByteArrayView encoded, bool plusIsSpace, QByteArray* out);

} // namespace qbucket::s3
