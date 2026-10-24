#include "platform/Console.h"

#include <QtGlobal>

#ifdef Q_OS_WIN
#include <windows.h>

#include <cstdio>
#endif

namespace qbucket::platform {

void attachParentConsole()
{
#ifdef Q_OS_WIN
    // Output already redirected (a pipe or a file) needs nothing.
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE* ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
#endif
}

} // namespace qbucket::platform
