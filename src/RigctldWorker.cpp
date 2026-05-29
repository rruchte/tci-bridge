#include "RigctldWorker.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>

RigctldWorker::RigctldWorker(QObject *parent)
	: QObject(parent)
{
}

RigctldWorker::~RigctldWorker()
{
	stop();
}

void RigctldWorker::start(QString host, quint16 port, int pollMs)
{
	host_ = host.trimmed();
	port_ = port;
	poll_ms_ = qMax(50, pollMs);

	qInfo() << "[RigctldWorker] starting:"
			<< host_ << port_
			<< "poll_ms=" << poll_ms_
			<< "thread=" << QThread::currentThread();

	if (!socket_)
		socket_ = new QTcpSocket(this);

	ensureConnected();

	if (!poll_timer_) {
		poll_timer_ = new QTimer(this);
		poll_timer_->setTimerType(Qt::CoarseTimer);

		connect(poll_timer_, &QTimer::timeout,
				this, &RigctldWorker::poll);
	}

	poll_timer_->setInterval(poll_ms_);
	poll_timer_->start();

	poll();
}

void RigctldWorker::stop()
{
	if (poll_timer_) {
		poll_timer_->stop();
		poll_timer_->deleteLater();
		poll_timer_ = nullptr;
	}

	if (socket_) {
		socket_->disconnectFromHost();

		if (socket_->state() != QAbstractSocket::UnconnectedState)
			socket_->waitForDisconnected(250);

		socket_->deleteLater();
		socket_ = nullptr;
	}

	if (connected_) {
		connected_ = false;
		emit connectedChanged(false);
	}
}

void RigctldWorker::setDebug(bool enabled)
{
	debug_ = enabled;
}

bool RigctldWorker::ensureConnected()
{
	if (!socket_)
		socket_ = new QTcpSocket(this);

	if (socket_->state() == QAbstractSocket::ConnectedState) {
		if (!connected_) {
			connected_ = true;
			emit connectedChanged(true);
		}

		return true;
	}

	socket_->abort();
	socket_->connectToHost(host_, port_);

	if (!socket_->waitForConnected(1000)) {
		const QString message =
			QStringLiteral("rigctld connection failed: %1")
				.arg(socket_->errorString());

		qWarning().noquote() << "[RigctldWorker]" << message;
		emit error(message);

		if (connected_) {
			connected_ = false;
			emit connectedChanged(false);
		}

		return false;
	}

	connected_ = true;
	emit connectedChanged(true);

	qInfo() << "[RigctldWorker] connected:"
			<< host_ << port_;

	return true;
}

QString RigctldWorker::transact(const QString &command,
								int timeoutMs,
								const QString &operation)
{
	return transactLines(command, timeoutMs, operation).join('\n').trimmed();
}

QStringList RigctldWorker::transactLines(const QString &command,
										 int timeoutMs,
										 const QString &operation)
{
	QStringList lines;

	if (!ensureConnected())
		return lines;

	if (socket_->bytesAvailable() > 0)
		socket_->readAll();

	QElapsedTimer totalTimer;
	totalTimer.start();

	const QByteArray payload = command.toUtf8() + '\n';

	if (debug_)
		qInfo().noquote() << "rigctld TX:" << command;

	socket_->write(payload);

	if (!socket_->waitForBytesWritten(timeoutMs)) {
		const QString message =
			QStringLiteral("%1 write failed: %2")
				.arg(operation, socket_->errorString());

		qWarning().noquote() << "[RigctldWorker]" << message;
		emit error(message);

		socket_->abort();

		if (connected_) {
			connected_ = false;
			emit connectedChanged(false);
		}

		return lines;
	}

	QByteArray buffer;

	while (totalTimer.elapsed() < timeoutMs) {
		const int remaining =
			timeoutMs - static_cast<int>(totalTimer.elapsed());

		if (remaining <= 0)
			break;

		if (!socket_->waitForReadyRead(remaining))
			break;

		buffer += socket_->readAll();

		// Set commands terminate with RPRT.
		if (buffer.contains("\nRPRT ") || buffer.startsWith("RPRT "))
			break;

		// Simple one-line queries.
		if ((command == "f" ||
			 command == "i" ||
			 command == "s" ||
			 command == "t") &&
			buffer.contains('\n')) {
			break;
		}

		// Mode query usually returns mode + passband.
		if (command == "m" && buffer.count('\n') >= 2)
			break;
	}

	const QList<QByteArray> rawLines = buffer.split('\n');

	for (const QByteArray &rawLine : rawLines) {
		const QString line = QString::fromUtf8(rawLine).trimmed();

		if (!line.isEmpty())
			lines << line;
	}

	if (debug_)
		qInfo().noquote() << "rigctld RX:" << lines.join(" | ");

	const qint64 elapsed = totalTimer.elapsed();

	if (elapsed > 100) {
		qWarning() << "[RigctldWorker] slow call:"
				   << operation
				   << elapsed << "ms";

		emit slowCall(operation, elapsed);
	}

	return lines;
}

bool RigctldWorker::transactRprt(const QString &command,
								 int timeoutMs,
								 const QString &operation)
{
	const QString response = transact(command, timeoutMs, operation);

	if (response.startsWith(QStringLiteral("RPRT 0")))
		return true;

	qWarning().noquote()
		<< "[RigctldWorker]" << operation
		<< "failed response:" << response;

	emit error(QStringLiteral("%1 failed: %2").arg(operation, response));

	return false;
}

bool RigctldWorker::transactOptional(const QString &command,
									 QString &response,
									 const QString &description)
{
	QElapsedTimer timer;
	timer.start();

	response = transact(command, 75, description);

	if (response.isEmpty()) {
		qWarning() << "[RigctldWorker] optional" << description
				   << "failed or timed out";
		return false;
	}

	const qint64 elapsed = timer.elapsed();

	if (elapsed > 75) {
		qWarning() << "[RigctldWorker] optional" << description
				   << "was slow:" << elapsed << "ms"
				   << "response:" << response;
	}

	return true;
}

void RigctldWorker::setFrequencyHz(qint64 hz)
{
	if (hz <= 0)
		return;

	if (!transactRprt(QStringLiteral("F %1").arg(hz),
					  1000,
					  QStringLiteral("set frequency"))) {
		return;
	}

	if (frequency_hz_ != hz) {
		frequency_hz_ = hz;
		emit frequencyChanged(hz);
	}
}

void RigctldWorker::setTxFrequencyHz(qint64 hz)
{
	if (hz <= 0)
		return;

	// Preserve your existing behavior: optimistic TX frequency shadow update,
	// then best-effort rigctld "I <hz>".
	tx_frequency_hz_ = hz;
	emit txFrequencyChanged(hz);

	const QString response =
		transact(QStringLiteral("I %1").arg(hz),
				 1000,
				 QStringLiteral("set TX frequency"));

	if (!response.startsWith(QStringLiteral("RPRT 0"))) {
		qWarning().noquote()
			<< "[RigctldWorker] failed to set TX frequency:"
			<< hz
			<< "response:" << response;

		emit error(QStringLiteral("failed to set TX frequency: %1").arg(response));
	}
}

void RigctldWorker::setSplitEnabled(bool enabled)
{
	split_enabled_ = enabled;
	emit splitChanged(enabled);

	if (!split_set_works_) {
		qWarning() << "[RigctldWorker] rigctld split set previously failed; ignoring split request";
		return;
	}

	const QString command =
		QStringLiteral("S %1 VFOB").arg(enabled ? 1 : 0);

	const QString response =
		transact(command,
				 1000,
				 QStringLiteral("set split"));

	if (!response.startsWith(QStringLiteral("RPRT 0"))) {
		split_set_works_ = false;

		qWarning().noquote()
			<< "[RigctldWorker] disabling split set; failed response:"
			<< response;

		emit error(QStringLiteral("set split failed: %1").arg(response));
	}
}

void RigctldWorker::setMode(QString mode)
{
	const QString rigMode = normalizeModeForRigctld(mode);

	if (rigMode.isEmpty())
		return;

	if (!transactRprt(QStringLiteral("M %1 0").arg(rigMode),
					  1000,
					  QStringLiteral("set mode"))) {
		return;
	}

	const QString normalized = normalizeModeFromRigctld(rigMode);

	if (mode_ != normalized) {
		mode_ = normalized;
		emit modeChanged(mode_);
	}
}

void RigctldWorker::setPtt(bool enabled)
{
	const QString response =
		transact(QStringLiteral("T %1").arg(enabled ? 1 : 0),
				 1000,
				 QStringLiteral("set PTT"));

	if (!response.startsWith(QStringLiteral("RPRT 0"))) {
		qWarning().noquote()
			<< "[RigctldWorker] rigctld failed to set PTT:"
			<< enabled
			<< "response:" << response;

		emit error(QStringLiteral("set PTT failed: %1").arg(response));
		return;
	}

	if (ptt_ != enabled) {
		ptt_ = enabled;
		emit pttChanged(enabled);
	}
}

void RigctldWorker::setPollingSuspended(bool suspended)
{
	if (polling_suspended_ == suspended)
		return;

	polling_suspended_ = suspended;

	qInfo() << "[RigctldWorker] polling"
			<< (polling_suspended_ ? "suspended" : "resumed");
}

void RigctldWorker::poll()
{
	if (polling_suspended_)
		return;

	QElapsedTimer timer;
	timer.start();

	pollFrequency();
	pollMode();
	pollPtt();

	if (!optional_state_polled_) {
		optional_state_polled_ = true;
		pollOptionalStateOnce();
	}

	const qint64 elapsed = timer.elapsed();

	if (elapsed > 100) {
		qWarning() << "[RigctldWorker] slow poll:"
				   << elapsed << "ms";

		emit slowCall(QStringLiteral("poll"), elapsed);
	}
}

void RigctldWorker::pollFrequency()
{
	const QString response =
		transact(QStringLiteral("f"),
				 1000,
				 QStringLiteral("poll frequency"));

	bool ok = false;
	const qint64 hz = response.trimmed().toLongLong(&ok);

	if (!ok || hz <= 0)
		return;

	if (frequency_hz_ != hz) {
		frequency_hz_ = hz;
		emit frequencyChanged(hz);
	}
}

void RigctldWorker::pollOptionalStateOnce()
{
	QElapsedTimer timer;
	timer.start();

	if (tx_freq_query_works_)
		pollTxFrequencyBestEffort();

	if (split_query_works_)
		pollSplitBestEffort();

	const qint64 elapsed = timer.elapsed();

	if (elapsed > 100) {
		qWarning() << "[RigctldWorker] slow optional state poll:"
				   << elapsed << "ms";
	}
}

void RigctldWorker::pollTxFrequencyBestEffort()
{
	if (!tx_freq_query_works_)
		return;

	QString response;

	if (!transactOptional(QStringLiteral("i"),
						  response,
						  QStringLiteral("TX frequency query"))) {
		tx_freq_query_works_ = false;
		return;
	}

	bool ok = false;
	const qint64 hz =
		response.trimmed().section('\n', 0, 0).toLongLong(&ok);

	if (!ok || hz <= 0) {
		qWarning() << "[RigctldWorker] disabling TX frequency polling;"
				   << "unsupported response:" << response;

		tx_freq_query_works_ = false;
		return;
	}

	if (tx_frequency_hz_ != hz) {
		tx_frequency_hz_ = hz;
		emit txFrequencyChanged(hz);
	}
}

void RigctldWorker::pollSplitBestEffort()
{
	if (!split_query_works_)
		return;

	QString response;

	if (!transactOptional(QStringLiteral("s"),
						  response,
						  QStringLiteral("split query"))) {
		split_query_works_ = false;
		return;
	}

	const QString firstLine = response.trimmed().section('\n', 0, 0);

	bool ok = false;
	const int enabledInt = firstLine.toInt(&ok);

	if (!ok) {
		qWarning() << "[RigctldWorker] disabling split polling;"
				   << "unsupported response:" << response;

		split_query_works_ = false;
		return;
	}

	const bool enabled = enabledInt != 0;

	if (split_enabled_ != enabled) {
		split_enabled_ = enabled;
		emit splitChanged(enabled);
	}
}

void RigctldWorker::pollMode()
{
	const QStringList lines =
		transactLines(QStringLiteral("m"),
					  1000,
					  QStringLiteral("poll mode"));

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

void RigctldWorker::pollPtt()
{
	const QString response =
		transact(QStringLiteral("t"),
				 1000,
				 QStringLiteral("poll PTT"));

	bool ok = false;
	const int value = response.trimmed().toInt(&ok);

	if (!ok)
		return;

	const bool enabled = value != 0;

	if (ptt_ != enabled) {
		ptt_ = enabled;
		emit pttChanged(enabled);
	}
}

QString RigctldWorker::normalizeModeForRigctld(const QString &mode)
{
	const QString m = mode.trimmed().toUpper();

	if (m == "DIGU" || m == "PKTUSB")
		return "PKTUSB";

	if (m == "DIGL" || m == "PKTLSB")
		return "PKTLSB";

	if (m == "USB" || m == "LSB" || m == "CW" || m == "CWR" ||
		m == "AM" || m == "FM" || m == "RTTY" || m == "RTTYR") {
		return m;
	}

	return m;
}

QString RigctldWorker::normalizeModeFromRigctld(const QString &mode)
{
	const QString m = mode.trimmed().toUpper();

	if (m == "PKTUSB")
		return "DIGU";

	if (m == "PKTLSB")
		return "DIGL";

	return m;
}