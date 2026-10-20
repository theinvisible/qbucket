#pragma once

#include <QLoggingCategory>

// Application logging categories. Enable at runtime with e.g.
//   QT_LOGGING_RULES="qbucket.*.debug=true"
// The S3 library brings its own `qbucket.s3.*` categories (M1).

Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcUi)
Q_DECLARE_LOGGING_CATEGORY(lcStall)
