#pragma once

#include "platform/VirtualFileSource.h"

#include <windows.h>
#include <objidl.h>
#include <shldisp.h>
#include <shlobj.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qbucket {

// The data of a virtual-file drag on Windows (PLAN.md §6 M7): Explorer asks
// for CFSTR_FILEDESCRIPTORW (names, sizes, dates; folders first) and then
// CFSTR_FILECONTENTS per file — a RangeReaderStream that downloads while
// Explorer reads. Also offers the in-app formats (objects of this QBucket,
// for drops on its own folders and buckets) and "copy" as the preferred
// effect. With IDataObjectAsyncCapability Explorer copies on a thread of
// its own and DoDragDrop returns at once.
//
// Lives in the process's MTA: every call (from DoDragDrop through a proxy,
// from Explorer through COM) arrives on an MTA thread and may block there;
// the GUI thread only holds a proxy.
class VirtualFileDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    struct Format {
        UINT format = 0;            // a registered clipboard format
        std::string bytes;          // TYMED_HGLOBAL
    };
    // Once: when Explorer ends the asynchronous copy (EndOperation), or when
    // the object goes without one (a cancelled drag, an in-app drop).
    using Finished = std::function<void(HRESULT result, DWORD effect)>;

    VirtualFileDataObject(std::shared_ptr<VirtualFileSource> source, std::vector<Format> formats, Finished finished);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override;
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override;
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink, DWORD* connection) override;
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override;
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** advise) override;
    // IDataObjectAsyncCapability
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL doAsync) override;
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* isAsync) override;
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx* reserved) override;
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* inAsyncOp) override;
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx* reserved, DWORD effects) override;

    // The clipboard formats it answers to (tests).
    [[nodiscard]] UINT descriptorFormat() const { return m_cfDescriptor; }
    [[nodiscard]] UINT contentsFormat() const { return m_cfContents; }

private:
    ~VirtualFileDataObject();
    HRESULT descriptor_(STGMEDIUM* medium);
    HRESULT contents_(LONG index, STGMEDIUM* medium);
    [[nodiscard]] bool offers_(const FORMATETC& f) const;
    void finish_(HRESULT result, DWORD effect);

    std::atomic<ULONG> m_refs{1};
    std::shared_ptr<VirtualFileSource> m_source;
    std::vector<Format> m_formats;
    UINT m_cfDescriptor = 0;
    UINT m_cfContents = 0;
    UINT m_cfPreferredEffect = 0;
    std::vector<FORMATETC> m_offered;

    std::mutex m_mutex;
    std::vector<VirtualFileSource::File> m_files;
    bool m_listed = false;
    struct Stored {
        FORMATETC format;
        std::string bytes;
    };
    std::vector<Stored> m_stored;   // what the drop target sets (drag image, performed effect)
    bool m_async = true;
    bool m_inOperation = false;
    Finished m_finished;
};

} // namespace qbucket
