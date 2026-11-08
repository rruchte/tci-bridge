#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// TCI 2.1 MODULATIONS_LIST conveys mode names, not a closed enum.
// Preserve valid rig-specific names; map only the packet-sideband aliases
// that this Hamlib bridge explicitly supports.
namespace TciModeNames
{
inline QString fromHamlib(const QString &value)
{
    const QString mode = value.trimmed().toUpper();
    if (mode == "PKTUSB") return QStringLiteral("DIGU");
    if (mode == "PKTLSB") return QStringLiteral("DIGL");
    return mode;
}

inline QString toHamlib(const QString &value)
{
    const QString mode = value.trimmed().toUpper();
    if (mode == "DIGU") return QStringLiteral("PKTUSB");
    if (mode == "DIGL") return QStringLiteral("PKTLSB");
    return mode;
}

inline QStringList parseHamlibSupportedModes(const QString &response)
{
    // Require the complete success response, never a partial or failed
    // capability discovery. Keep the original order and distinct modes.
    const QStringList lines = response.trimmed().split('\n', Qt::SkipEmptyParts);
    if (lines.size() != 2 || lines.at(1).trimmed() != QStringLiteral("RPRT 0"))
        return {};

    const QStringList tokens = lines.at(0).trimmed().toUpper().split(
        QRegularExpression(QStringLiteral("[,\\s]+")),
        Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return {};

    static const QRegularExpression validName(
        QStringLiteral("^[A-Z0-9][A-Z0-9_-]{0,31}$"));

    QStringList modes;
    for (const QString &token : tokens) {
        if (!validName.match(token).hasMatch())
            return {};
        const QString tciMode = fromHamlib(token);
        if (!modes.contains(tciMode))
            modes.append(tciMode);
    }
    return modes;
}
}
