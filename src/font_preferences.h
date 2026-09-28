#pragma once

#include <QString>
#include <QStringList>

namespace FontPreferences {

QString installedFamily(const QStringList &candidates);
QString systemSerifFamily();
QString dropCapFamily(const QString &style);

}
