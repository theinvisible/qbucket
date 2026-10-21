#include "platform/win/VirtualFileDataObject.h"

#include "platform/win/RangeReaderStream.h"

#include <algorithm>
#include <cstring>

namespace qbucket {

namespace {

// {3D8B0590-F691-11d2-8EA9-006097DF5BD4}: IDataObjectAsyncCapability (formerly IAsyncOperation).
const IID kIidAsyncCapability = {0x3D8B0590, 0xF691, 0x11d2, {0x8E, 0xA9, 0x00, 0x60, 0x97, 0xDF, 0x5B, 0xD4}};

FORMATETC formatOf(UINT cf, DWORD tymed, LONG index = -1)
{
    FORMATETC f;
    f.cfFormat = CLIPFORMAT(cf);
    f.ptd = nullptr;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = index;
    f.tymed = tymed;
    return f;
}

HRESULT hglobalOf(const void* bytes, size_t size, STGMEDIUM* medium)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, std::max<size_t>(size, 1));
    if (!h)
        return E_OUTOFMEMORY;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return E_OUTOFMEMORY;
    }
    if (size > 0)
        std::memcpy(p, bytes, size);
    GlobalUnlock(h);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = h;
    medium->pUnkForRelease = nullptr;
    return S_OK;
}

FILETIME fileTimeOf(std::int64_t msecsSinceEpoch)
{
    // 100-ns intervals since 1601-01-01.
    const ULONGLONG ticks = ULONGLONG(msecsSinceEpoch) * 10000ULL + 116444736000000000ULL;
    FILETIME t;
    t.dwLowDateTime = DWORD(ticks & 0xFFFFFFFFULL);
    t.dwHighDateTime = DWORD(ticks >> 32);
    return t;
}

} // namespace

VirtualFileDataObject::VirtualFileDataObject(std::shared_ptr<VirtualFileSource> source, std::vector<Format> formats,
                                             Finished finished)
    : m_source(std::move(source)),
      m_formats(std::move(formats)),
      m_cfDescriptor(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
      m_cfContents(RegisterClipboardFormatW(CFSTR_FILECONTENTS)),
      m_cfPreferredEffect(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),
      m_finished(std::move(finished))
{
    m_offered.push_back(formatOf(m_cfDescriptor, TYMED_HGLOBAL));
    m_offered.push_back(formatOf(m_cfContents, TYMED_ISTREAM));
    m_offered.push_back(formatOf(m_cfPreferredEffect, TYMED_HGLOBAL));
    for (const Format& f : m_formats)
        m_offered.push_back(formatOf(f.format, TYMED_HGLOBAL));
}

VirtualFileDataObject::~VirtualFileDataObject()
{
    // No asynchronous copy ran (or it never ended): the drag is over.
    finish_(S_OK, DROPEFFECT_NONE);
}

void VirtualFileDataObject::finish_(HRESULT result, DWORD effect)
{
    Finished done;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        done = std::move(m_finished);
        m_finished = nullptr;
    }
    if (done)
        done(result, effect);
}

// --- IUnknown ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
        *object = static_cast<IDataObject*>(this);
    } else if (IsEqualIID(riid, kIidAsyncCapability)) {
        *object = static_cast<IDataObjectAsyncCapability*>(this);
    } else {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE VirtualFileDataObject::AddRef()
{
    return ++m_refs;
}

ULONG STDMETHODCALLTYPE VirtualFileDataObject::Release()
{
    const ULONG left = --m_refs;
    if (left == 0)
        delete this;
    return left;
}

// --- IDataObject ------------------------------------------------------------------------

bool VirtualFileDataObject::offers_(const FORMATETC& f) const
{
    if (f.dwAspect != DVASPECT_CONTENT)
        return false;
    return std::any_of(m_offered.begin(), m_offered.end(), [&f](const FORMATETC& o) {
        return o.cfFormat == f.cfFormat && (o.tymed & f.tymed) != 0;
    });
}

HRESULT VirtualFileDataObject::descriptor_(STGMEDIUM* medium)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_listed) {
            // Blocks until the selection is listed — on this MTA thread.
            if (m_source->files(&m_files) != VirtualFileSource::Status::Ok)
                return E_FAIL;
            m_listed = true;
        }
    }
    const size_t count = m_files.size();
    const size_t bytes = sizeof(FILEGROUPDESCRIPTORW) + (count > 0 ? count - 1 : 0) * sizeof(FILEDESCRIPTORW);
    std::vector<char> buffer(bytes, 0);
    auto* group = reinterpret_cast<FILEGROUPDESCRIPTORW*>(buffer.data());
    group->cItems = UINT(count);
    for (size_t i = 0; i < count; ++i) {
        const VirtualFileSource::File& f = m_files[i];
        FILEDESCRIPTORW& d = group->fgd[i];
        d.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
        d.dwFileAttributes = f.folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (!f.folder) {
            d.dwFlags |= FD_FILESIZE;
            d.nFileSizeLow = DWORD(ULONGLONG(f.size) & 0xFFFFFFFFULL);
            d.nFileSizeHigh = DWORD(ULONGLONG(f.size) >> 32);
        }
        if (f.modifiedMsecs > 0) {
            d.dwFlags |= FD_WRITESTIME;
            d.ftLastWriteTime = fileTimeOf(f.modifiedMsecs);
        }
        if (f.path.size() >= MAX_PATH)
            return E_FAIL;   // Explorer can't take it; the reader refuses such names first
        std::memcpy(d.cFileName, f.path.c_str(), (f.path.size() + 1) * sizeof(wchar_t));
    }
    return hglobalOf(buffer.data(), buffer.size(), medium);
}

HRESULT VirtualFileDataObject::contents_(LONG index, STGMEDIUM* medium)
{
    std::int64_t size = 0;
    std::u16string name;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_listed || index < 0 || size_t(index) >= m_files.size() || m_files[size_t(index)].folder)
            return DV_E_LINDEX;
        size = m_files[size_t(index)].size;
        name = m_files[size_t(index)].path;
    }
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = new RangeReaderStream(m_source, size_t(index), size, name);
    medium->pUnkForRelease = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::GetData(FORMATETC* format, STGMEDIUM* medium)
{
    if (!format || !medium)
        return E_INVALIDARG;
    std::memset(medium, 0, sizeof(*medium));
    if (format->cfFormat == m_cfDescriptor && (format->tymed & TYMED_HGLOBAL))
        return descriptor_(medium);
    if (format->cfFormat == m_cfContents && (format->tymed & TYMED_ISTREAM))
        return contents_(format->lindex, medium);
    if (format->cfFormat == m_cfPreferredEffect && (format->tymed & TYMED_HGLOBAL)) {
        const DWORD copy = DROPEFFECT_COPY;
        return hglobalOf(&copy, sizeof(copy), medium);
    }
    if (format->tymed & TYMED_HGLOBAL) {
        for (const Format& f : m_formats) {
            if (f.format == format->cfFormat)
                return hglobalOf(f.bytes.data(), f.bytes.size(), medium);
        }
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (const Stored& s : m_stored) {
            if (s.format.cfFormat == format->cfFormat)
                return hglobalOf(s.bytes.data(), s.bytes.size(), medium);
        }
    }
    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::GetDataHere(FORMATETC*, STGMEDIUM*)
{
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::QueryGetData(FORMATETC* format)
{
    if (!format)
        return E_INVALIDARG;
    if (offers_(*format))
        return S_OK;
    const std::lock_guard<std::mutex> lock(m_mutex);
    for (const Stored& s : m_stored) {
        if (s.format.cfFormat == format->cfFormat && (format->tymed & TYMED_HGLOBAL))
            return S_OK;
    }
    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out)
{
    if (out)
        out->ptd = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release)
{
    // The shell stores things here (drag image, performed effect): kept as
    // bytes, handed back by GetData.
    if (!format || !medium)
        return E_INVALIDARG;
    if (medium->tymed != TYMED_HGLOBAL)
        return E_NOTIMPL;
    const size_t size = GlobalSize(medium->hGlobal);
    const void* p = GlobalLock(medium->hGlobal);
    Stored s{*format, std::string(static_cast<const char*>(p), p ? size : 0)};
    if (p)
        GlobalUnlock(medium->hGlobal);
    s.format.ptd = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stored.erase(std::remove_if(m_stored.begin(), m_stored.end(),
                                      [&](const Stored& x) { return x.format.cfFormat == format->cfFormat; }),
                       m_stored.end());
        m_stored.push_back(std::move(s));
    }
    if (release)
        ReleaseStgMedium(medium);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats)
{
    if (!formats)
        return E_POINTER;
    if (direction != DATADIR_GET) {
        *formats = nullptr;
        return E_NOTIMPL;
    }
    return SHCreateStdEnumFmtEtc(UINT(m_offered.size()), m_offered.data(), formats);
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::DUnadvise(DWORD)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::EnumDAdvise(IEnumSTATDATA**)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

// --- IDataObjectAsyncCapability --------------------------------------------------------

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::SetAsyncMode(BOOL doAsync)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_async = doAsync != FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::GetAsyncMode(BOOL* isAsync)
{
    if (!isAsync)
        return E_INVALIDARG;
    const std::lock_guard<std::mutex> lock(m_mutex);
    *isAsync = m_async ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::StartOperation(IBindCtx*)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_inOperation = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::InOperation(BOOL* inAsyncOp)
{
    if (!inAsyncOp)
        return E_INVALIDARG;
    const std::lock_guard<std::mutex> lock(m_mutex);
    *inAsyncOp = m_inOperation ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileDataObject::EndOperation(HRESULT result, IBindCtx*, DWORD effects)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_inOperation = false;
    }
    finish_(result, effects);
    return S_OK;
}

} // namespace qbucket
