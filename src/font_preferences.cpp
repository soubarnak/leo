#include "font_preferences.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>

namespace FontPreferences {

QString installedFamily(const QStringList &candidates)
{
    const QStringList installedFonts = QFontDatabase::families();
    for (const QString &candidate : candidates) {
        for (const QString &installed : installedFonts) {
            if (installed.compare(candidate, Qt::CaseInsensitive) == 0) {
                return installed;
            }
        }
    }
    return {};
}

QString systemSerifFamily()
{
    QFont font;
    font.setStyleHint(QFont::Serif);
    return QFontInfo(font).family();
}

QString bodyFallbackFamily()
{
    const QString installed = installedFamily({QStringLiteral("Georgia"),
                                               QStringLiteral("Liberation Serif"),
                                               QStringLiteral("DejaVu Serif"),
                                               QStringLiteral("Noto Serif")});
    return installed.isEmpty() ? systemSerifFamily() : installed;
}

QString dropCapFamily(const QString &style)
{
    if (style == QStringLiteral("fantasy")) {
        return installedFamily({QStringLiteral("Apple Chancery"),
                                QStringLiteral("Snell Roundhand"),
                                QStringLiteral("URW Chancery L"),
                                QStringLiteral("DejaVu Serif")});
    }
    if (style == QStringLiteral("scifi")) {
        return installedFamily({QStringLiteral("Futura"), QStringLiteral("Avenir Next"),
                                QStringLiteral("Helvetica Neue"), QStringLiteral("DejaVu Sans"),
                                QStringLiteral("Liberation Sans")});
    }
    return installedFamily({QStringLiteral("Didot"), QStringLiteral("Bodoni 72"),
                            QStringLiteral("Georgia"), QStringLiteral("Liberation Serif"),
                            QStringLiteral("DejaVu Serif")});
}

}
