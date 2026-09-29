#pragma once

#include "legacy_chapter_codec.h"

LegacyChapterLinkContext loadChapterLinks(const QString &libraryPath,
                                          const QString &bookId,
                                          const QString &chapterId);
