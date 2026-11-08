#include "RigctldWorker.h"

#include "TciModeNames.h"
#include "TciFrequencyCaps.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QRegularExpression>
#include <QSet>

#include <limits>
#include <cmath>

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

    modes_queried_ = false;
    vfo_limits_queried_ = false;
    emit supportedModesChanged({});
    emit vfoLimitsChanged(0, 0);
    setRadioUsable(false, QStringLiteral("worker stopped"));

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
		const QString message = QStringLiteral("rigctld connection failed: %1")
			.arg(socket_->errorString());

		qWarning().noquote() << "[RigctldWorker]" << message;
		emit error(message);

		modes_queried_ = false;
		vfo_limits_queried_ = false;
		emit supportedModesChanged({});
		emit vfoLimitsChanged(0, 0);
		setRadioUsable(false, message);

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
		if (buffer.endsWith('\n') &&
		    (buffer.contains("\nRPRT ") || buffer.startsWith("RPRT ")))
			break;

		// Simple one-line queries.
		if ((command == "f" ||
			 command == "i" || command == "j" || command == "z" ||
			 command.startsWith("u ") || command.startsWith("l ") ||
			 command == "t") &&
			buffer.contains('\n')) {
			break;
		}

		// Mode and split return two lines (mode + passband, split + TX VFO).
		if ((command == "m" || command == "s") && buffer.count('\n') >= 2)
			break;
	}

	// An unterminated line may be a prefix of a longer result.
	if (!buffer.endsWith('\n'))
		return {};

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
								 const QString &operation, bool *modeRejected)
{
	const QString response = transact(command, timeoutMs, operation);
	if (modeRejected) {
		*modeRejected = response == "RPRT -1" || response == "RPRT -4" ||
		                response == "RPRT -9" || response == "RPRT -11";
		if (*modeRejected)
			return false;
	}

	if (response == QStringLiteral("RPRT 0"))
		return true;

	// A delayed response must not be consumed by the next CAT command.
	if (!response.startsWith(QStringLiteral("RPRT "))) {
		setRadioUsable(false, operation);
		forceReconnect(operation);
	}

	qWarning().noquote()
		<< "[RigctldWorker]" << operation
		<< "failed response:" << response;

	emit error(QStringLiteral("%1 failed: %2").arg(operation, response));

	return false;
}

bool RigctldWorker::transactOptional(const QString &command,
									 QString &response,
									 const QString &description, int timeoutMs)
{
	QElapsedTimer timer;
	timer.start();

	response = transact(command, timeoutMs, description);

	if (response.isEmpty() || (command == "s" && !response.startsWith("RPRT ") &&
	                          !response.contains('\n'))) {
		// Discard the connection so a late optional reply cannot become
		// the reply to the next CAT command.
		socket_->abort();
		qWarning() << "[RigctldWorker] optional" << description
				   << "failed or timed out";
		return false;
	}

	const qint64 elapsed = timer.elapsed();

	if (elapsed > timeoutMs) {
		qWarning() << "[RigctldWorker] optional" << description
				   << "was slow:" << elapsed << "ms"
				   << "response:" << response;
	}

	return true;
}

namespace
{
struct RxControlCommand {
	const char *read;
	const char *write;
};

constexpr std::array<RxControlCommand, rxControlCount> rxControlCommands{{
	{"u RIT", "U RIT"},
	{"j", "J"},
	{"u XIT", "U XIT"},
	{"z", "Z"},
	{"u NB", "U NB"},
	{"u NR", "U NR"},
}};
}

void RigctldWorker::pollRxControl(RxControl setting, bool force)
{
	const auto index = static_cast<int>(setting);
	if (rx_control_unsupported_[index] && !force)
		return;

	const auto &command = rxControlCommands[index];
	QString response;
	bool ok = false;
	int value = 0;
	if (transactOptional(QString::fromLatin1(command.read), response, QStringLiteral("read RX control"),
	                     force ? 750 : 75))
		value = response.toInt(&ok);
	if (rxControlIsSwitch(setting)) {
		if (setting == RxControl::NoiseBlanker || setting == RxControl::NoiseReduction) {
			ok = ok && value >= 0;
			value = value != 0;
		} else if (value != 0 && value != 1) {
			ok = false;
		}
	}

	// Hamlib ENIMPL and ENAVAIL. Retry other errors on later polls.
	rx_control_unsupported_[index] = response == "RPRT -4" || response == "RPRT -11";
	const auto current = ok ? std::optional<int>(value) : std::nullopt;
	if (force || rx_controls_[index] != current) {
		rx_controls_[index] = current;
		emit rxControlChanged(setting, value, ok);
	}
}

void RigctldWorker::readRxControl(RxControl setting)
{
	if (radio_usable_ && !ptt_ && !polling_suspended_)
		pollRxControl(setting, true);
}

void RigctldWorker::setRxControl(RxControl setting, int value)
{
	if (!radio_usable_ || ptt_ || polling_suspended_)
		return;

	const auto &command = rxControlCommands[static_cast<int>(setting)];
	if (!transactRprt(QStringLiteral("%1 %2").arg(QString::fromLatin1(command.write)).arg(value),
	                  1000, QStringLiteral("set RX control")))
		return;

	pollRxControl(setting, true);
	// Some Yaesu radios share one offset register between RIT and XIT.
	if (setting == RxControl::RitOffset)
		pollRxControl(RxControl::XitOffset);
	if (setting == RxControl::XitOffset)
		pollRxControl(RxControl::RitOffset);
}

void RigctldWorker::setTxMeterInterval(int milliseconds)
{
	tx_meter_interval_ms_ = milliseconds;
	tx_meter_clock_.invalidate();
}

std::optional<double> RigctldWorker::readTxMeter(const QString &level, bool &unsupported)
{
	if (unsupported)
		return {};

	QString response;
	if (!transactOptional(QStringLiteral("l %1").arg(level), response, level))
		return {};

	unsupported = response == "RPRT -4" || response == "RPRT -11";
	bool ok = false;
	const double value = response.toDouble(&ok);
	if (ok && std::isfinite(value) && value >= 0.0)
		return value;
	return {};
}

void RigctldWorker::pollTxMeter()
{
	if (!tx_meter_interval_ms_ || !ptt_)
		return;
	if (tx_meter_clock_.isValid() && tx_meter_clock_.elapsed() < tx_meter_interval_ms_)
		return;

	tx_meter_clock_.start();
	TxMeterReading reading;
	reading.outputWatts = readTxMeter(QStringLiteral("RFPOWER_METER_WATTS"), tx_watts_unsupported_);
	reading.swr = readTxMeter(QStringLiteral("SWR"), tx_swr_unsupported_);
	if (reading.swr && *reading.swr < 1.0)
		reading.swr.reset();
	if (reading.outputWatts || reading.swr)
		emit txMeterReading(reading);
}

void RigctldWorker::setRxMeterInterval(int milliseconds)
{
	rx_meter_interval_ms_ = milliseconds;
	rx_meter_clock_.invalidate();
}

void RigctldWorker::pollRxMeter()
{
	if (!rx_meter_interval_ms_ || rx_meter_unsupported_ || frequency_hz_ <= 0)
		return;
	if (rx_meter_clock_.isValid() && rx_meter_clock_.elapsed() < rx_meter_interval_ms_)
		return;

	rx_meter_clock_.start();
	QString response;
	if (!transactOptional(QStringLiteral("l STRENGTH"), response, QStringLiteral("read RX meter")))
		return;

	rx_meter_unsupported_ = response == "RPRT -4" || response == "RPRT -11";
	bool ok = false;
	const int relativeDb = response.toInt(&ok);
	if (!ok || relativeDb < -200 || relativeDb > 150)
		return;

	// Hamlib STRENGTH is dB relative to S9. Use the IARU S9 reference
	// for the tuned RF frequency. The result is an estimate.
	const int s9Dbm = frequency_hz_ < 30000000 ? -73 : -93;
	emit rxSignalLevel(s9Dbm + relativeDb);
}

void RigctldWorker::setFilterEdgesHzEnabled(bool enabled)
{
	filter_edges_hz_enabled_ = enabled;
	if (!enabled)
		updateRxFilterBand({});
}

QPair<int, int> RigctldWorker::readFilterSlopes(int timeoutMs)
{
	if (!filter_edges_hz_enabled_ || (mode_ != "USB" && mode_ != "LSB"))
		return {};

	QString reply;
	bool lowOk = false;
	bool highOk = false;
	if (!transactOptional(QStringLiteral("l SLOPE_LOW"), reply,
	                      QStringLiteral("read low filter slope"), timeoutMs))
		return {};
	const int low = reply.toInt(&lowOk);
	if (!lowOk || low < 0)
		return {};
	if (!transactOptional(QStringLiteral("l SLOPE_HIGH"), reply,
	                      QStringLiteral("read high filter slope"), timeoutMs))
		return {};
	const int high = reply.toInt(&highOk);
	if (!highOk || low >= high)
		return {};
	return {low, high};
}

void RigctldWorker::updateRxFilterBand(QPair<int, int> slopes, bool force)
{
	if (mode_ == "LSB")
		slopes = {-slopes.second, -slopes.first};
	if (force || rx_filter_band_ != slopes) {
		rx_filter_band_ = slopes;
		emit rxFilterBandChanged(slopes.first, slopes.second);
	}
}

void RigctldWorker::readRxFilterBand()
{
	if (filter_edges_hz_enabled_ && radio_usable_ && !ptt_ && !polling_suspended_ && pollMode())
		updateRxFilterBand(readFilterSlopes(500), true);
}

void RigctldWorker::setRxFilterBand(int low, int high)
{
	if (!filter_edges_hz_enabled_ || !radio_usable_ || ptt_ || polling_suspended_ ||
	    !pollMode() || low >= high)
		return;

	if (mode_ == "LSB" && high <= 0 && low != std::numeric_limits<int>::min()) {
		const int audioLow = -high;
		high = -low;
		low = audioLow;
	} else if (mode_ != "USB" || low < 0) {
		return;
	}

	const auto current = readFilterSlopes(500);
	if (current.first >= current.second) {
		updateRxFilterBand({});
		return;
	}

	// Move the outer edge first if the new band is above the old band.
	const bool highFirst = low >= current.second;
	const auto setEdge = [this](const char *edge, int currentValue, int value) {
		return currentValue == value ||
		       transactRprt(QStringLiteral("L %1 %2").arg(QString::fromLatin1(edge)).arg(value),
		                    500, QStringLiteral("set filter slope"));
	};
	if (highFirst) {
		if (setEdge("SLOPE_HIGH", current.second, high))
			setEdge("SLOPE_LOW", current.first, low);
	} else if (setEdge("SLOPE_LOW", current.first, low)) {
		setEdge("SLOPE_HIGH", current.second, high);
	}

	// The first write may succeed even when the second fails. Publish the
	// actual pair, including any quantization or partial change.
	if (radio_usable_)
		updateRxFilterBand(readFilterSlopes(500), true);
}

bool RigctldWorker::pollAgc(bool force, QString *observed)
{
	if (agc_unsupported_ && !force)
		return false;

	QString response;
	QString mode;
	bool ok = false;
	if (transactOptional(QStringLiteral("l AGC"), response, QStringLiteral("read AGC"),
	                     force ? 750 : 250)) {
		// Hamlib AGC: off, superfast, fast, slow, user, medium, auto.
		static const QStringList tciModes{"off", "fast", "fast", "normal", QString(), "normal", "normal"};
		const int level = response.toInt(&ok);
		if (ok)
			mode = tciModes.value(level);
	}

	agc_unsupported_ = response == "RPRT -4" || response == "RPRT -11";
	if (observed) {
		*observed = mode;
	} else if (force || agc_mode_ != mode) {
		agc_mode_ = mode;
		emit agcModeChanged(mode);
	}
	return ok;
}

void RigctldWorker::readAgc()
{
	if (radio_usable_ && !ptt_ && !polling_suspended_)
		pollAgc(true);
}

void RigctldWorker::setAgcMode(const QString &mode)
{
	if (!radio_usable_ || ptt_ || polling_suspended_)
		return;

	// Preserve a rig's current native mode if it already maps to the request.
	QString current;
	if (!pollAgc(true, &current))
		return;
	if (current == mode) {
		agc_mode_ = current;
		emit agcModeChanged(current);
		return;
	}

	QList<int> candidates;
	if (mode == "normal")
		candidates = {6, 5, 3}; // AUTO, MEDIUM, SLOW
	else if (mode == "fast")
		candidates = {2, 1}; // FAST, SUPERFAST
	else if (mode == "off")
		candidates = {0};
	else
		return;

	for (int candidate : candidates) {
		bool rejected = false;
		if (transactRprt(QStringLiteral("L AGC %1").arg(candidate), 1000,
		                 QStringLiteral("set AGC mode"), &rejected)) {
			pollAgc(true);
			return;
		}
		if (!rejected)
			return;
	}

	qWarning() << "Rig declined all Hamlib AGC modes for TCI mode" << mode;
	pollAgc(true);
}

void RigctldWorker::pollDrive(bool force)
{
	if (drive_unsupported_ && !force)
		return;

	QString response;
	int percent = -1;
	if (transactOptional(QStringLiteral("l RFPOWER"), response, QStringLiteral("read RF power"),
	                     force ? 750 : 250)) {
		bool ok = false;
		const double fraction = response.toDouble(&ok);
		if (ok && fraction >= 0.0 && fraction <= 1.0)
			percent = qRound(fraction * 100.0);
	}

	const bool unsupported = response == "RPRT -4" || response == "RPRT -11";
	if (percent < 0 && (force || (unsupported && !drive_unsupported_)))
		qWarning().noquote() << "No valid DRIVE readback from rigctld:" << response;
	drive_unsupported_ = unsupported;
	if (force || drive_percent_ != percent) {
		drive_percent_ = percent;
		emit driveChanged(percent);
	}
}

void RigctldWorker::readDrive()
{
	if (!radio_usable_ || ptt_ || polling_suspended_)
		return;
	pollDrive(true);
}

void RigctldWorker::setDrivePercent(int percent)
{
	if (!radio_usable_ || ptt_ || polling_suspended_ || percent < 0 || percent > 100) {
		qWarning() << "DRIVE write skipped: CAT unavailable, transmitting or invalid value";
		return;
	}

	// Recheck an earlier unsupported result before allowing a write.
	if (drive_unsupported_) {
		pollDrive(true);
		if (drive_percent_ < 0)
			return;
	}

	const QString value = QString::number(percent / 100.0, 'f', 2);
	if (transactRprt(QStringLiteral("L RFPOWER %1").arg(value), 1000,
	                 QStringLiteral("set RF power")))
		pollDrive(true);
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

	frequency_hz_ = hz;
	emit frequencyChanged(hz);
}

void RigctldWorker::setTxFrequencyHz(qint64 hz)
{
	if (hz <= 0)
		return;

	if (transactRprt(QStringLiteral("I %1").arg(hz),
	                 1000, QStringLiteral("set TX frequency"))) {
		tx_frequency_hz_ = hz;
		emit txFrequencyChanged(hz);
	}
}

void RigctldWorker::setSplitEnabled(bool enabled)
{
	if (transactRprt(QStringLiteral("S %1 VFOB").arg(enabled ? 1 : 0),
	                 1000, QStringLiteral("set split"))) {
		if (enabled && !split_enabled_ && !polling_suspended_)
			pollTxFrequencyBestEffort();
		split_enabled_ = enabled;
		split_known_ = true;
		emit splitChanged(enabled);
	}
}

void RigctldWorker::setMode(QString mode)
{
	const QString rigMode = normalizeModeForRigctld(mode);
	static const QRegularExpression validMode(QStringLiteral("\\A[A-Z0-9][A-Z0-9_-]{0,31}\\z"));

	if (!validMode.match(rigMode).hasMatch())
		return;

	if (!transactRprt(QStringLiteral("M %1 0").arg(rigMode),
					  1000,
					  QStringLiteral("set mode"))) {
		return;
	}

	const QString normalized = normalizeModeFromRigctld(rigMode);

	if (mode_ != normalized)
		updateRxFilterBand({});
	mode_ = normalized;
	emit modeChanged(mode_);
}

void RigctldWorker::setPtt(bool enabled)
{
	if (!transactRprt(QStringLiteral("T %1").arg(enabled ? 1 : 0),
	                  1000, QStringLiteral("set PTT")) ||
	    !pollPtt() || ptt_ != enabled) {
		emit pttCommandFailed(enabled);
		return;
	}

	emit pttCommandSucceeded(enabled);
}

void RigctldWorker::setPollingSuspended(bool suspended)
{
	if (polling_suspended_ == suspended)
		return;

	polling_suspended_ = suspended;

	qInfo() << "[RigctldWorker] polling"
			<< (polling_suspended_ ? "suspended" : "resumed");
}

void RigctldWorker::setRadioUsable(bool usable, const QString &reason)
{
	if (radio_usable_ == usable) return;

	radio_usable_ = usable;
	if (!usable) {
		split_poll_clock_.invalidate();
		rx_control_poll_clock_.invalidate();
		next_rx_control_ = 0;
		drive_percent_ = -1;
		drive_unsupported_ = false;
		agc_unsupported_ = false;
		agc_mode_.clear();
		rx_filter_band_ = {};
		rx_meter_unsupported_ = false;
		tx_watts_unsupported_ = false;
		tx_swr_unsupported_ = false;
		tx_meter_clock_.invalidate();
		rx_meter_clock_.invalidate();
		rx_controls_ = {};
		rx_control_unsupported_ = {};
		split_known_ = false;
		tx_frequency_hz_ = 0;
		split_query_works_ = true;
	}

	if (usable) {
		qInfo().noquote() << "[RigctldWorker] radio usable";
	} else {
		qWarning().noquote() << "[RigctldWorker] radio not usable"
							 << (reason.isEmpty() ? QString() : QStringLiteral("reason=%1").arg(reason));
	}

	emit radioUsableChanged(usable);
}

void RigctldWorker::notePollSuccess()
{
	consecutive_poll_failures_ = 0;
	setRadioUsable(true);
}

void RigctldWorker::notePollFailure(const QString &operation, const QString &response)
{
	++consecutive_poll_failures_;

	qWarning().noquote()
		<< "[RigctldWorker] poll failure"
		<< operation
		<< "count=" << consecutive_poll_failures_
		<< "response=" << response;

	if (consecutive_poll_failures_ >= max_consecutive_poll_failures_) {
		setRadioUsable(false, QStringLiteral("%1 failed %2 times")
			.arg(operation)
			.arg(consecutive_poll_failures_));

		forceReconnect(QStringLiteral("%1 failed repeatedly").arg(operation));
		consecutive_poll_failures_ = 0;
	}
}

void RigctldWorker::forceReconnect(const QString &reason)
{
	qWarning().noquote() << "[RigctldWorker] forcing rigctld TCP reconnect:" << reason;
	modes_queried_ = false;
	vfo_limits_queried_ = false;
	emit supportedModesChanged({});
	emit vfoLimitsChanged(0, 0);

	if (socket_) {
		socket_->abort();
	}

	if (connected_) {
		connected_ = false;
		emit connectedChanged(false);
	}
}

void RigctldWorker::poll()
{
	if (polling_suspended_) {
		if (pollPtt()) {
			notePollSuccess();
			pollTxMeter();
		}
		return;
	}

	QElapsedTimer timer;
	timer.start();

	const bool freqOk = pollFrequency();
	const bool modeOk = pollMode();
	const bool pttOk = pollPtt();

	if (freqOk && modeOk && pttOk) {
		notePollSuccess();
	}

	if (pttOk && ptt_ && radio_usable_)
		pollTxMeter();

	if (freqOk && pttOk && !ptt_ && radio_usable_)
		pollRxMeter();

	if (!ptt_ && radio_usable_ && (!split_poll_clock_.isValid() || split_poll_clock_.elapsed() >= 1000)) {
		pollSplitBestEffort();
		split_poll_clock_.start();
	}

	if (!ptt_ && radio_usable_ &&
	    (!rx_control_poll_clock_.isValid() || rx_control_poll_clock_.elapsed() >= 225)) {
		rx_control_poll_clock_.start();
		// Poll one optional RX control per interval to bound CAT latency.
		if (next_rx_control_ == rx_controls_.size())
			pollDrive();
		else if (next_rx_control_ == rx_controls_.size() + 1)
			pollAgc();
		else if (next_rx_control_ == rx_controls_.size() + 2)
			updateRxFilterBand(readFilterSlopes(125));
		else
			pollRxControl(static_cast<RxControl>(next_rx_control_));
		next_rx_control_ = (next_rx_control_ + 1) % (rx_controls_.size() + (filter_edges_hz_enabled_ ? 3 : 2));
	}

	if (!modes_queried_ && radio_usable_) {
		modes_queried_ = true;
		querySupportedModes();
	}

	if (!vfo_limits_queried_ && radio_usable_) {
		vfo_limits_queried_ = true;
		queryVfoLimits();
	}

	const qint64 elapsed = timer.elapsed();
	if (elapsed > 100) {
		qWarning() << "[RigctldWorker] slow poll:" << elapsed << "ms";
		emit slowCall(QStringLiteral("poll"), elapsed);
	}
}

bool RigctldWorker::pollFrequency()
{
	const QString response = transact(QStringLiteral("f"), 1000, QStringLiteral("poll frequency"));

	bool ok = false;
	const qint64 hz = response.trimmed().toLongLong(&ok);

	if (!ok || hz <= 0) {
		notePollFailure(QStringLiteral("poll frequency"), response);
		return false;
	}

	if (frequency_hz_ != hz) {
		frequency_hz_ = hz;
		emit frequencyChanged(hz);
	}

	return true;
}

void RigctldWorker::queryVfoLimits()
{
	// dump_state has no short reply delimiter. Use an independent temporary
	// connection so a timeout can never desynchronize CAT polling.
	QTcpSocket capabilitySocket;
	capabilitySocket.connectToHost(host_, port_);
	if (!capabilitySocket.waitForConnected(350)) {
		qWarning() << "[RigctldWorker] VFO capability connection failed:"
		           << capabilitySocket.errorString();
		return;
	}
	capabilitySocket.write(QByteArrayLiteral("\\dump_state\n"));
	if (!capabilitySocket.waitForBytesWritten(250))
		return;

	QByteArray output;
	QElapsedTimer elapsed;
	elapsed.start();
	while (elapsed.elapsed() < 1300 && output.size() < 65536) {
		if (!capabilitySocket.waitForReadyRead(150))
			break;
		output += capabilitySocket.readAll();
	}
	output += capabilitySocket.readAll();

	const auto limits = TciFrequencyCaps::fromDumpState(
		QString::fromUtf8(output));
	if (!limits.valid()) {
		const QStringList lines = QString::fromUtf8(output).split(
			'\n', Qt::SkipEmptyParts);
		qWarning() << "[RigctldWorker] VFO limits unavailable:"
		           << output.size() << "bytes of dump_state"
		           << "lines=" << lines.size()
		           << "first-lines=" << lines.mid(0, 6)
		           << "zero-markers="
		           << lines.count(QStringLiteral("0 0 0 0 0 0 0"));
		return;
	}
	qInfo() << "[RigctldWorker] discovered VFO limits:"
	        << limits.minHz << limits.maxHz;
	emit vfoLimitsChanged(limits.minHz, limits.maxHz);
}

void RigctldWorker::querySupportedModes()
{
	// M ? exposes the rig's mode names. The TCI 2.1 dictionary does not
	// define a closed set of names, so retain distinct Hamlib modes.
	const QString response = transact(QStringLiteral("M ?"), 400,
	                                  QStringLiteral("query supported modes"));
	const QStringList modes = TciModeNames::parseHamlibSupportedModes(response);
	if (modes.isEmpty()) {
		qWarning() << "[RigctldWorker] invalid supported-mode response:" << response;
		return;
	}

	qInfo() << "[RigctldWorker] discovered supported modes:" << modes;
	emit supportedModesChanged(modes);
}

void RigctldWorker::pollTxFrequencyBestEffort()
{
	QString response;
	bool ok = false;
	qint64 hz = 0;
	if (transactOptional(QStringLiteral("i"), response, QStringLiteral("TX frequency query")))
		hz = response.toLongLong(&ok);
	if (!ok || hz <= 0)
		hz = 0;

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
		split_known_ = false;
		emit splitStateUnavailable();
		return;
	}

	const QString firstLine = response.trimmed().section('\n', 0, 0);

	bool ok = false;
	const int enabledInt = firstLine.toInt(&ok);

	if (!ok || (enabledInt != 0 && enabledInt != 1)) {
		qWarning() << "[RigctldWorker] disabling split polling;"
				   << "unsupported response:" << response;

		split_query_works_ = false;
		split_known_ = false;
		emit splitStateUnavailable();
		return;
	}

	const bool enabled = enabledInt != 0;

	if (enabled || !split_known_)
		pollTxFrequencyBestEffort();

	if (!split_known_ || split_enabled_ != enabled) {
		split_known_ = true;
		split_enabled_ = enabled;
		emit splitChanged(enabled);
	}
}

bool RigctldWorker::pollMode()
{
	const QStringList lines = transactLines(QStringLiteral("m"), 1000, QStringLiteral("poll mode"));

	if (lines.isEmpty()) {
		notePollFailure(QStringLiteral("poll mode"), QStringLiteral("<empty>"));
		return false;
	}

	const QString newMode = normalizeModeFromRigctld(lines.at(0));

	if (newMode.isEmpty()) {
		notePollFailure(QStringLiteral("poll mode"), lines.join(QStringLiteral(" | ")));
		return false;
	}

	if (mode_ != newMode) {
		updateRxFilterBand({});
		mode_ = newMode;
		emit modeChanged(mode_);
	}

	return true;
}

bool RigctldWorker::pollPtt()
{
	const QString response = transact(QStringLiteral("t"), 1000, QStringLiteral("poll PTT"));

	bool ok = false;
	const int value = response.trimmed().toInt(&ok);

	if (!ok) {
		notePollFailure(QStringLiteral("poll PTT"), response);
		return false;
	}

	const bool enabled = value != 0;

	if (ptt_ != enabled) {
		tx_meter_clock_.invalidate();
		ptt_ = enabled;
		emit pttChanged(enabled);
	}

	return true;
}

QString RigctldWorker::normalizeModeForRigctld(const QString &mode)
{
	return TciModeNames::toHamlib(mode);
}

QString RigctldWorker::normalizeModeFromRigctld(const QString &mode)
{
	return TciModeNames::fromHamlib(mode);
}
