#pragma once

namespace qbucket::platform {

// A Windows GUI executable has no console: its output to stdout / stderr
// goes nowhere, even when started from cmd or PowerShell. For the command
// line options (--version, --self-test) it attaches to the console of the
// process that started it, if there is one. Elsewhere it does nothing.
void attachParentConsole();

} // namespace qbucket::platform
