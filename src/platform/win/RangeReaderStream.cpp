#include "platform/win/RangeReaderStream.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace qbucket {

RangeReaderStream::RangeReaderStream(std::shared_ptr<VirtualFileSource> source, std::size_t index, std::int64_t size,
                                     std::u16string name)
    : m_source(std::move(source)),
      m_index(index),
      m_size(size),
      m_name(std::move(name))
{
}

RangeReaderStream::~RangeReaderStream() = default;

HRESULT STDMETHODCALLTYPE RangeReaderStream::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) || IsEqualIID(riid, IID_ISequentialStream)) {
        *object = static_cast<IStream*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RangeReaderStream::AddRef()
{
    return ++m_refs;
}

ULONG STDMETHODCALLTYPE RangeReaderStream::Release()
{
    const ULONG left = --m_refs;
    if (left == 0)
        delete this;
    return left;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Read(void* buffer, ULONG length, ULONG* read)
{
    ULONG total = 0;
    auto* out = static_cast<char*>(buffer);
    while (total < length && m_position < m_size) {
        std::uint32_t got = 0;
        const VirtualFileSource::Status s =
            m_source->read(m_index, m_position, out + total, std::uint32_t(length - total), &got);
        if (s != VirtualFileSource::Status::Ok) {
            if (read)
                *read = total;
            return s == VirtualFileSource::Status::Cancelled ? E_ABORT : STG_E_READFAULT;
        }
        if (got == 0)
            break;
        total += got;
        m_position += got;
    }
    if (read)
        *read = total;
    // S_FALSE: fewer bytes than asked for (the end).
    return total < length ? S_FALSE : S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Write(const void*, ULONG, ULONG*)
{
    return STG_E_ACCESSDENIED;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position)
{
    std::int64_t base = 0;
    switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = m_position; break;
    case STREAM_SEEK_END: base = m_size; break;
    default: return STG_E_INVALIDFUNCTION;
    }
    const std::int64_t next = base + move.QuadPart;
    if (next < 0)
        return STG_E_INVALIDFUNCTION;
    m_position = next;
    if (position)
        position->QuadPart = ULONGLONG(m_position);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::SetSize(ULARGE_INTEGER)
{
    return STG_E_ACCESSDENIED;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                                    ULARGE_INTEGER* written)
{
    if (!target)
        return STG_E_INVALIDPOINTER;
    constexpr ULONG kStep = 1024 * 1024;
    std::vector<char> buffer(kStep);
    ULONGLONG left = count.QuadPart;
    ULONGLONG in = 0;
    ULONGLONG out = 0;
    HRESULT hr = S_OK;
    while (left > 0) {
        ULONG got = 0;
        hr = Read(buffer.data(), ULONG(std::min<ULONGLONG>(left, kStep)), &got);
        if (FAILED(hr))
            break;
        in += got;
        ULONG put = 0;
        if (got > 0) {
            hr = target->Write(buffer.data(), got, &put);
            out += put;
            if (FAILED(hr))
                break;
        }
        if (got == 0 || hr == S_FALSE)
            break;
        left -= got;
    }
    if (read)
        read->QuadPart = in;
    if (written)
        written->QuadPart = out;
    return FAILED(hr) ? hr : S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Commit(DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Revert()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
{
    return STG_E_INVALIDFUNCTION;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD)
{
    return STG_E_INVALIDFUNCTION;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Stat(STATSTG* stat, DWORD flags)
{
    if (!stat)
        return STG_E_INVALIDPOINTER;
    std::memset(stat, 0, sizeof(*stat));
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = ULONGLONG(m_size);
    stat->grfMode = STGM_READ;
    if (!(flags & STATFLAG_NONAME)) {
        const size_t bytes = (m_name.size() + 1) * sizeof(wchar_t);
        stat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
        if (!stat->pwcsName)
            return E_OUTOFMEMORY;
        std::memcpy(stat->pwcsName, m_name.c_str(), bytes);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RangeReaderStream::Clone(IStream** stream)
{
    if (!stream)
        return STG_E_INVALIDPOINTER;
    auto* copy = new RangeReaderStream(m_source, m_index, m_size, m_name);
    copy->m_position = m_position;
    *stream = copy;
    return S_OK;
}

} // namespace qbucket
