#include "http/UriEncoding.h"

namespace qbucket::s3 {
namespace {

constexpr bool isUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

QByteArray uriEncode(QByteArrayView bytes, SlashMode slash)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    QByteArray out;
    out.reserve(bytes.size() * 3);
    for (const char c : bytes) {
        if (isUnreserved(c) || (c == '/' && slash == SlashMode::Keep)) {
            out.append(c);
        } else {
            const auto u = static_cast<unsigned char>(c);
            out.append('%');
            out.append(hex[u >> 4]);
            out.append(hex[u & 0x0F]);
        }
    }
    return out;
}

bool uriDecode(QByteArrayView encoded, bool plusIsSpace, QByteArray* out)
{
    QByteArray result;
    result.reserve(encoded.size());
    for (qsizetype i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '%') {
            if (i + 2 >= encoded.size())
                return false;
            const int hi = hexValue(encoded[i + 1]);
            const int lo = hexValue(encoded[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            result.append(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            result.append(' ');
        } else {
            result.append(c);
        }
    }
    *out = result;
    return true;
}

} // namespace qbucket::s3
