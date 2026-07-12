#ifndef QSARIO_H
#define QSARIO_H

#include "domain/QsarProduct.h"

class QsarIO
{
public:
    static bool write(const QString& filePath, const QsarProduct& product);
    static QsarProduct read(const QString& filePath);

    static QString lastError() { return mLastError; }

private:
    static QString mLastError;
};

#endif // QSARIO_H
