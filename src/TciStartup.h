#pragma once

#include <QStringList>

// Bridge-wide facts that can be stated without asking the radio backend
// for capabilities it does not currently expose. Only TCI protocol messages
// are emitted here; no bridge-specific negotiation extensions.
namespace TciStartup
{
inline QStringList messages(bool transmitEnabled)
{
    return {
        QStringLiteral("protocol:tci-bridge,2.1;"),
        QStringLiteral("device:tci-bridge;"),
        transmitEnabled
            ? QStringLiteral("receive_only:false;")
            : QStringLiteral("receive_only:true;"),
        QStringLiteral("trx_count:1;"),
        QStringLiteral("channels_count:1;"),
        QStringLiteral("ready;")
    };
}
}
