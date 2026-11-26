#pragma once

#include <QDir>
#include <QFileInfo>

inline bool isDatabaseFile(const QString &database, const QString &candidate) {
    const QFileInfo file(candidate);
    auto output = file.canonicalFilePath();
    if (output.isEmpty())
        output = QDir(file.dir().canonicalPath()).absoluteFilePath(file.fileName());
    const auto source = QFileInfo(database).canonicalFilePath();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    for (const auto *suffix : {"", "-wal", "-shm", "-journal"})
        if (output.compare(source + suffix, pathCase) == 0)
            return true;
    return false;
}
