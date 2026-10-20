#pragma once

// QtKeychain's header lives under a different directory depending on where
// the library came from (cmake/QBucketQtKeychain.cmake). Include this one.
#ifdef QBUCKET_QTKEYCHAIN_FETCHED
#include <qtkeychain/keychain.h>
#else
#include <qt6keychain/keychain.h>
#endif
