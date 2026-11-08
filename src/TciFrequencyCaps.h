#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace TciFrequencyCaps
{
struct Limits
{
    qint64 minHz = 0;
    qint64 maxHz = 0;
    bool valid() const { return minHz > 0 && maxHz > minHz; }
};

// Hamlib dump_state is a positional protocol, not a general-purpose
// key/value capability API. Interpret only the beginning of its legacy
// layout: banner, three numeric fields, zero-terminated RX and TX ranges.
// Fail closed if data is partial, malformed, or has discontinuous RX bands.
inline Limits fromDumpState(const QString &response)
{
    const QStringList lines = response.split('\n', Qt::SkipEmptyParts);

    // Hamlib NET rigctl may display a "rigctld:" banner in its output,
    // while a direct connection can return only the positional state lines.
    // Support both, but never infer where range data starts by guessing.
    const int offset = !lines.isEmpty() &&
                       lines.first().startsWith(QStringLiteral("rigctld:"))
                       ? 1 : 0;
    if (lines.size() < offset + 6)
        return {};

    for (int i = offset; i < offset + 3; ++i) {
        bool ok = false;
        lines.at(i).trimmed().toInt(&ok);
        if (!ok)
            return {};
    }

    const QRegularExpression space(QStringLiteral("\\s+"));
    const auto parseRange = [&](const QString &line, Limits *range,
                                bool *terminator) -> bool {
        const QStringList t = line.trimmed().split(space, Qt::SkipEmptyParts);
        if (t.size() != 7)
            return false;
        bool startOk = false, endOk = false;
        const double start = t.at(0).toDouble(&startOk);
        const double end = t.at(1).toDouble(&endOk);
        if (!startOk || !endOk || !std::isfinite(start) || !std::isfinite(end))
            return false;
        if (start == 0 && end == 0) {
            // A complete seven-field zero marker is required.
            for (const QString &field : t)
                if (field != "0")
                    return false;
            *terminator = true;
            return true;
        }
        if (start < 1 || end <= start ||
            end > static_cast<double>(std::numeric_limits<qint64>::max()) ||
            std::trunc(start) != start || std::trunc(end) != end)
            return false;
        // Check the other five fields are present and syntactically usable.
        for (int i = 2; i < 7; ++i) {
            const QString token = t.at(i);
            bool ok = false;
            if (token.startsWith("0x", Qt::CaseInsensitive))
                token.mid(2).toULongLong(&ok, 16);
            else
                token.toLongLong(&ok);
            if (!ok)
                return false;
        }
        *range = { static_cast<qint64>(start), static_cast<qint64>(end) };
        return true;
    };

    QVector<Limits> rx;
    int index = offset + 3;
    bool foundRxEnd = false;
    for (; index < lines.size() && rx.size() < 100; ++index) {
        Limits range;
        bool marker = false;
        if (!parseRange(lines.at(index), &range, &marker))
            return {};
        if (marker) {
            foundRxEnd = true;
            ++index;
            break;
        }
        rx.append(range);
    }
    if (!foundRxEnd || rx.isEmpty())
        return {};

    // Require a complete, terminated TX section too, so a truncated
    // network response cannot accidentally yield plausible RX limits.
    bool foundTxEnd = false;
    for (int count = 0; index < lines.size() && count < 100; ++index, ++count) {
        Limits range;
        bool marker = false;
        if (!parseRange(lines.at(index), &range, &marker))
            return {};
        if (marker) {
            foundTxEnd = true;
            break;
        }
    }
    if (!foundTxEnd)
        return {};

    std::sort(rx.begin(), rx.end(), [](const Limits &a, const Limits &b) {
        return a.minHz < b.minHz;
    });
    Limits merged = rx.first();
    for (int i = 1; i < rx.size(); ++i) {
        if (rx.at(i).minHz > merged.maxHz)
            return {}; // TCI VFO_LIMITS cannot represent gaps.
        merged.maxHz = std::max(merged.maxHz, rx.at(i).maxHz);
    }
    return merged;
}
}
