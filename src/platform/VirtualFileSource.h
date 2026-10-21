#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qbucket {

// What a virtual-file drag (Windows: CFSTR_FILEDESCRIPTORW /
// CFSTR_FILECONTENTS) hands to the file manager: the files and folders of
// the dragged selection and their bytes, read on demand. Plain C++ on
// purpose: the COM layer (src/platform/win) depends on nothing else.
//
// Thread-safety: every call is thread-safe and may block (a listing, a
// download) — never call it on the GUI thread.
class VirtualFileSource {
public:
    struct File {
        std::u16string path;              // relative, "\"-separated; safe for Windows
        bool folder = false;
        std::int64_t size = 0;
        std::int64_t modifiedMsecs = 0;   // UTC, since the epoch; 0: unknown
    };
    enum class Status { Ok, Failed, Cancelled };

    virtual ~VirtualFileSource() = default;

    // The entries, folders before what is in them. Blocks until the
    // selection is listed.
    virtual Status files(std::vector<File>* out) = 0;
    // Up to `length` bytes of entry `index` from `offset`; *read == 0 at the
    // end. Sequential reads are fast (read ahead) and checked against the
    // object's ETag at the end where that is an MD5; others work too.
    virtual Status read(std::size_t index, std::int64_t offset, void* buffer, std::uint32_t length,
                        std::uint32_t* read) = 0;
    // Wakes every waiting call; later calls answer Cancelled.
    virtual void cancel() = 0;
    // Why the last call failed, for the user.
    [[nodiscard]] virtual std::u16string lastError() const = 0;
};

} // namespace qbucket
