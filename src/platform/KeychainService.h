#pragma once

#include <functional>

namespace qbucket::platform {

// Whether a keychain service exists at all, asked without blocking.
//
// QtKeychain on Linux reports a missing keyring daemon as an ordinary error
// ("Unknown error", or the D-Bus error's text) — never as "no backend" — so a
// failed keychain call can't tell a broken keychain from none. This answers
// it: on the session bus, the Secret Service (org.freedesktop.secrets) or
// KWallet, running or activatable; inside a Flatpak also the Secret portal.
// No session bus: none. On Windows and macOS there always is one (true).
//
// `done` runs on the calling thread's event loop, never synchronously.
// Thread affinity: a thread with an event loop (the GUI thread).
void checkKeychainService(std::function<void(bool exists)> done);

} // namespace qbucket::platform
