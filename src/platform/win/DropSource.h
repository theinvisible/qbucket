#pragma once

#include <windows.h>
#include <ole2.h>

#include <atomic>

namespace qbucket {

// The IDropSource of a virtual-file drag: Escape cancels, releasing the
// mouse button drops, the cursors are the system's.
class DropSource final : public IDropSource {
public:
    DropSource() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

private:
    ~DropSource() = default;
    std::atomic<ULONG> m_refs{1};
};

} // namespace qbucket
