#pragma once

#include "domain/Location.h"

#include <QString>
#include <QUuid>

namespace qbucket {

// A named place in the browser (account + bucket + prefix). Plain value.
struct Bookmark {
    QUuid id;
    QString name;
    Location location;
};

} // namespace qbucket
