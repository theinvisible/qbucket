#include "platform/win/VirtualFileDrag.h"

#include "controllers/AccountSessions.h"
#include "controllers/OperationController.h"
#include "platform/VirtualFileReader.h"
#include "platform/win/DropSource.h"
#include "platform/win/VirtualFileDataObject.h"

#include <QCoreApplication>
#include <QMimeData>
#include <QPointer>

#include <thread>

namespace qbucket {

VirtualFileDrag::VirtualFileDrag(OperationController* operations, QObject* parent)
    : ExternalDrag(parent),
      m_ops(operations)
{
    // The MTA must outlive the thread that creates a drag's data object.
    if (FAILED(CoIncrementMTAUsage(&m_mta)))
        m_mta = nullptr;
}

VirtualFileDrag::~VirtualFileDrag()
{
    if (m_mta)
        CoDecrementMTAUsage(m_mta);
}

IDataObject* VirtualFileDrag::createDataObject(std::shared_ptr<VirtualFileReader> reader, const QMimeData* internal,
                                               std::function<void(HRESULT, DWORD)> finished)
{
    std::vector<VirtualFileDataObject::Format> formats;
    if (internal) {
        for (const QString& mime : internal->formats()) {
            // Qt's drop targets look custom MIME types up by their name.
            const UINT cf = RegisterClipboardFormatW(reinterpret_cast<const wchar_t*>(mime.utf16()));
            const QByteArray bytes = internal->data(mime);
            if (cf != 0)
                formats.push_back({cf, std::string(bytes.constData(), size_t(bytes.size()))});
        }
    }
    IStream* marshaled = nullptr;
    HRESULT hr = E_FAIL;
    // Created on an MTA thread: its calls never come to the GUI thread.
    std::thread mta([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return;
        auto* object = new VirtualFileDataObject(std::move(reader), std::move(formats), std::move(finished));
        hr = CoMarshalInterThreadInterfaceInStream(IID_IDataObject, static_cast<IDataObject*>(object), &marshaled);
        object->Release();   // the marshaled reference keeps it
        CoUninitialize();
    });
    mta.join();
    if (FAILED(hr) || !marshaled)
        return nullptr;
    IDataObject* proxy = nullptr;
    if (FAILED(CoGetInterfaceAndReleaseStream(marshaled, IID_IDataObject, reinterpret_cast<void**>(&proxy))))
        return nullptr;
    return proxy;
}

Qt::DropAction VirtualFileDrag::exec(QObject* source, QMimeData* internal, const DragStaging::Selection& selection,
                                     Qt::DropActions actions, Qt::DropAction defaultAction)
{
    Q_UNUSED(source);
    Q_UNUSED(defaultAction);
    const std::unique_ptr<QMimeData> owned(internal);
    const s3::Result<s3::Session*> session = m_ops->sessions()->session(selection.account);
    if (!session || !m_mta) {
        emit message(unavailable(text(Text::NotConnected)));
        return Qt::IgnoreAction;
    }
    auto reader = std::make_shared<VirtualFileReader>(*session, selection);
    const QPointer<VirtualFileDrag> self(this);
    // Explorer reports the end of its copy on a COM thread: the summary goes
    // to the GUI thread.
    const auto finished = [self, reader](HRESULT result, DWORD effect) {
        const QStringList lines = reader->summary();
        const QString error = QString::fromStdU16String(reader->lastError());
        QMetaObject::invokeMethod(qApp, [self, lines, error, result, effect] {
            if (!self)
                return;
            qsizetype verified = 0;
            qsizetype failed = 0;
            for (const QString& l : lines) {
                verified += l.endsWith(QLatin1String(": md5 verified")) ? 1 : 0;
                failed += l.contains(QLatin1String(": failed")) ? 1 : 0;
            }
            if (failed > 0 || FAILED(result))
                emit self->message(ExternalDrag::failed(error.isEmpty() ? text(Text::CopyNotCompleted) : error));
            else if (!lines.isEmpty())
                emit self->message(done(int(lines.size()), int(verified)));
            Q_UNUSED(effect);
        }, Qt::QueuedConnection);
    };
    IDataObject* data = createDataObject(reader, owned.get(), finished);
    if (!data) {
        emit message(unavailable(text(Text::NoDragData)));
        return Qt::IgnoreAction;
    }
    auto* dropSource = new DropSource;
    DWORD allowed = DROPEFFECT_COPY;
    if (actions & Qt::MoveAction)
        allowed |= DROPEFFECT_MOVE;   // in-app drops with Shift
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = DoDragDrop(data, dropSource, allowed, &effect);
    dropSource->Release();
    data->Release();
    if (hr == DRAGDROP_S_CANCEL)
        reader->cancel();
    if (hr != DRAGDROP_S_DROP)
        return Qt::IgnoreAction;
    return (effect & DROPEFFECT_MOVE) ? Qt::MoveAction : (effect & DROPEFFECT_COPY) ? Qt::CopyAction : Qt::IgnoreAction;
}

} // namespace qbucket
