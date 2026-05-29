#include "RigctldBackend.h"

#include <QDebug>
#include <QElapsedTimer>

RigctldBackend::RigctldBackend(
    QString host,
    quint16 port,
    int pollMs,
    QObject *parent
)
    : RadioBackend(parent),
      host_(std::move(host)),
      port_(port),
      poll_ms_(pollMs)
{
    connect(&poll_timer_, &QTimer::timeout,
            this, &RigctldBackend::poll);

    poll_timer_.setInterval(poll_ms_);
    poll_timer_.start();

    qInfo() << "Rigctld backend configured:"
            << host_ << port_
            << "poll_ms=" << poll_ms_;

    poll();
}

qint64 RigctldBackend::frequencyHz() const
{
    return frequency_hz_;
}

void RigctldBackend::setFrequencyHz(qint64 hz)
{
    if (hz <= 0)
        return;

    const QString response = transact(QStringLiteral("F %1").arg(hz));

    if (!response.startsWith("RPRT 0")) {
        qWarning().noquote() << "rigctld failed to set frequency:"
                             << hz << "response:" << response;
        return;
    }

    if (frequency_hz_ != hz) {
        frequency_hz_ = hz;
        emit frequencyChanged(frequency_hz_);
    }
}

QString RigctldBackend::mode() const
{
    return mode_;
}

void RigctldBackend::setMode(const QString &mode)
{
    const QString rigMode = normalizeModeForRigctld(mode);

    if (rigMode.isEmpty())
        return;

    // Passband 0 asks rigctld/Hamlib to use the backend default.
    const QString response = transact(QStringLiteral("M %1 0").arg(rigMode));

    if (!response.startsWith("RPRT 0")) {
        qWarning().noquote() << "rigctld failed to set mode:"
                             << rigMode << "response:" << response;
        return;
    }

    const QString normalized = normalizeModeFromRigctld(rigMode);

    if (mode_ != normalized) {
        mode_ = normalized;
        emit modeChanged(mode_);
    }
}

bool RigctldBackend::ptt() const
{
    return ptt_;
}

void RigctldBackend::setPtt(bool enabled)
{
    const QString response = transact(QStringLiteral("T %1").arg(enabled ? 1 : 0));

    if (!response.startsWith("RPRT 0")) {
        qWarning().noquote() << "rigctld failed to set PTT:"
                             << enabled << "response:" << response;
        return;
    }

    if (ptt_ != enabled) {
        ptt_ = enabled;
        emit pttChanged(ptt_);
    }
}

void RigctldBackend::poll()
{
    pollFrequency();
    pollMode();
    pollPtt();
}

QString RigctldBackend::transact(const QString &command, int timeoutMs)
{
    const QStringList lines = transactLines(command, timeoutMs);
    return lines.join('\n').trimmed();
}

QStringList RigctldBackend::transactLines(const QString &command, int timeoutMs)
{
    QTcpSocket socket;
    QStringList lines;

    socket.connectToHost(host_, port_);

    if (!socket.waitForConnected(timeoutMs)) {
        qWarning() << "rigctld connection failed:"
                   << socket.errorString();
        return lines;
    }

    const QByteArray payload = command.toUtf8() + "\n";

	if (debug_)
	{
		qInfo().noquote() << "rigctld TX:" << command;
	}

    socket.write(payload);

    if (!socket.waitForBytesWritten(timeoutMs)) {
        qWarning() << "rigctld write failed:"
                   << socket.errorString();
        return lines;
    }

    QElapsedTimer timer;
    timer.start();

    QByteArray buffer;

    while (timer.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());

        if (remaining <= 0)
            break;

        if (!socket.waitForReadyRead(remaining))
            break;

        buffer += socket.readAll();

        // rigctld simple protocol responses are short. For set commands,
        // RPRT terminates the response. For get commands, one or two lines
        // are usually enough and rigctld stops sending.
        if (buffer.contains("\nRPRT ") || buffer.startsWith("RPRT "))
            break;

        if (command == "f" && buffer.contains('\n'))
            break;

        if (command == "m" && buffer.count('\n') >= 2)
            break;

        if (command == "t" && buffer.contains('\n'))
            break;
    }

    const QList<QByteArray> rawLines = buffer.split('\n');

    for (const QByteArray &rawLine : rawLines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();

        if (!line.isEmpty())
            lines << line;
    }

    socket.disconnectFromHost();

	if (debug_)
	{
		qInfo().noquote() << "rigctld RX:" << lines.join(" | ");
	}

    return lines;
}

void RigctldBackend::pollFrequency()
{
    const QString response = transact("f");

    bool ok = false;
    const qint64 hz = response.trimmed().toLongLong(&ok);

    if (!ok || hz <= 0)
        return;

    if (frequency_hz_ != hz) {
        frequency_hz_ = hz;
        emit frequencyChanged(frequency_hz_);
    }
}

void RigctldBackend::pollMode()
{
    const QStringList lines = transactLines("m");

    if (lines.isEmpty())
        return;

    const QString newMode = normalizeModeFromRigctld(lines.at(0));

    if (newMode.isEmpty())
        return;

    if (mode_ != newMode) {
        mode_ = newMode;
        emit modeChanged(mode_);
    }
}

void RigctldBackend::pollPtt()
{
    const QString response = transact("t");

    bool ok = false;
    const int value = response.trimmed().toInt(&ok);

    if (!ok)
        return;

    const bool enabled = value != 0;

    if (ptt_ != enabled) {
        ptt_ = enabled;
        emit pttChanged(ptt_);
    }
}

QString RigctldBackend::normalizeModeForRigctld(const QString &mode)
{
    const QString m = mode.trimmed().toUpper();

    if (m == "DIGU" || m == "PKTUSB")
        return "PKTUSB";

    if (m == "DIGL" || m == "PKTLSB")
        return "PKTLSB";

    if (m == "USB" || m == "LSB" || m == "CW" || m == "CWR" ||
        m == "AM" || m == "FM" || m == "RTTY" || m == "RTTYR")
        return m;

    return m;
}

QString RigctldBackend::normalizeModeFromRigctld(const QString &mode)
{
    const QString m = mode.trimmed().toUpper();

    if (m == "PKTUSB")
        return "DIGU";

    if (m == "PKTLSB")
        return "DIGL";

    return m;
}

void RigctldBackend::setDebug(bool enabled)
{
	debug_ = enabled;
}