#pragma once

#include "platform/VirtualFileSource.h"

#include <windows.h>
#include <objidl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace qbucket {

// The IStream Explorer reads one dragged file from (CFSTR_FILECONTENTS):
// Read() asks the VirtualFileSource, which blocks while the bytes are
// downloaded — on the COM thread Explorer's call arrives on (the MTA),
// never on the GUI thread. A failure is STG_E_READFAULT; Explorer then
// reports the copy as failed.
class RangeReaderStream final : public IStream {
public:
    RangeReaderStream(std::shared_ptr<VirtualFileSource> source, std::size_t index, std::int64_t size,
                      std::u16string name);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // ISequentialStream
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG length, ULONG* read) override;
    HRESULT STDMETHODCALLTYPE Write(const void* buffer, ULONG length, ULONG* written) override;
    // IStream
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override;
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override;
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override;
    HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Revert() override;
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type) override;
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER count, DWORD type) override;
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Clone(IStream** stream) override;

private:
    ~RangeReaderStream();

    std::atomic<ULONG> m_refs{1};
    std::shared_ptr<VirtualFileSource> m_source;
    std::size_t m_index;
    std::int64_t m_size;
    std::int64_t m_position = 0;
    std::u16string m_name;
};

} // namespace qbucket
