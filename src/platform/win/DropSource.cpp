#include "platform/win/DropSource.h"

namespace qbucket {

HRESULT STDMETHODCALLTYPE DropSource::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropSource)) {
        *object = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DropSource::AddRef()
{
    return ++m_refs;
}

ULONG STDMETHODCALLTYPE DropSource::Release()
{
    const ULONG left = --m_refs;
    if (left == 0)
        delete this;
    return left;
}

HRESULT STDMETHODCALLTYPE DropSource::QueryContinueDrag(BOOL escapePressed, DWORD keyState)
{
    if (escapePressed)
        return DRAGDROP_S_CANCEL;
    if (!(keyState & (MK_LBUTTON | MK_RBUTTON)))
        return DRAGDROP_S_DROP;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DropSource::GiveFeedback(DWORD)
{
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

} // namespace qbucket
