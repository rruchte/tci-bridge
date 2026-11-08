#include "TciServer.h"
#include "TciStartup.h"

#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr std::array<const char *, rxControlCount> rxControlNames{
	"rit_enable", "rit_offset", "xit_enable", "xit_offset", "rx_nb_enable", "rx_nr_enable"
};

QString rxControlMessage(RxControl setting, int value)
{
	const auto index = static_cast<int>(setting);
	const bool enable = rxControlIsSwitch(setting);
	const QString argument = enable ? (value ? QStringLiteral("true") : QStringLiteral("false"))
	                                : QString::number(value);
	return QStringLiteral("%1:0,%2;").arg(QString::fromLatin1(rxControlNames[index]), argument);
}
}

TciServer::TciServer(RadioBackend *radio, QObject *parent) :
	QObject(parent), server_(QStringLiteral("tci-bridge"), QWebSocketServer::NonSecureMode, this), radio_(radio)
{
	connect(&server_, &QWebSocketServer::newConnection, this, &TciServer::onNewConnection);

	connect(radio_, &RadioBackend::txMeterReading, this, &TciServer::sendTxMeterReading);
	connect(radio_, &RadioBackend::rxSignalLevel, this, &TciServer::sendRxSignalLevel);

	connect(radio_, &RadioBackend::rxFilterBandChanged, this, [this](int low, int high) {
		if (low < high)
			broadcastText(QStringLiteral("rx_filter_band:0,%1,%2;").arg(low).arg(high));
	});

	connect(radio_, &RadioBackend::agcModeChanged, this, [this](const QString &mode) {
		if (!mode.isEmpty())
			broadcastText(QStringLiteral("agc_mode:0,%1;").arg(mode));
	});

	connect(radio_, &RadioBackend::driveChanged, this, [this](int percent) {
		if (percent >= 0)
			broadcastText(QStringLiteral("drive:0,%1;").arg(percent));
	});

	connect(radio_, &RadioBackend::rxControlChanged, this,
	        [this](RxControl setting, int value, bool known) {
		        if (known)
			        broadcastText(rxControlMessage(setting, value));
		        if (setting == RxControl::XitEnabled || setting == RxControl::XitOffset)
			        broadcastTxFrequency();
	        });

	connect(radio_, &RadioBackend::frequencyChanged, this, &TciServer::onFrequencyChanged);

	connect(radio_, &RadioBackend::txFrequencyChanged,this, &TciServer::onTxFrequencyChanged);

	connect(radio_, &RadioBackend::splitChanged, this, &TciServer::onSplitChanged);

	connect(radio_, &RadioBackend::effectiveTxFrequencyInvalidated,
	        this, &TciServer::broadcastTxFrequency);

	connect(radio_, &RadioBackend::modeChanged, this, &TciServer::onModeChanged);

	connect(radio_, &RadioBackend::pttChanged, this, &TciServer::onPttChanged);

	connect(radio_, &RadioBackend::onlineChanged, this, &TciServer::onRadioOnlineChanged);

	connect(radio_, &RadioBackend::vfoLimitsChanged, this,
	        [this](qint64 minHz, qint64 maxHz) {
		        if (minHz > 0 && maxHz > minHz)
			        broadcastText(QStringLiteral("vfo_limits:%1,%2;")
			                      .arg(minHz).arg(maxHz));
	        });

	connect(radio_, &RadioBackend::supportedModesChanged, this,
	        [this](const QStringList &modes) {
		        if (modes.isEmpty())
			        return;
		        const QString announcement =
			        QStringLiteral("modulations_list:%1;").arg(modes.join(','));
		        broadcastText(announcement);
	        });

	connect(radio_, &RadioBackend::pttCommandFailed, this, [this](bool requested) {
		if (requested && tx_owner_) {
			stopTransmit(QStringLiteral("CAT key failed"), true);
		} else if (!requested && unkey_pending_) {
			QTimer::singleShot(250, this, [this]() {
				if (unkey_pending_ && radio_->online())
					radio_->setPtt(false);
			});
		}
	});

	connect(radio_, &RadioBackend::pttCommandSucceeded, this, [this](bool requested) {
		if (requested && tx_audio_owner_ && !unkey_pending_ && radio_->ptt()) {
			radio_->setPollingSuspended(true);
			const auto &state = audio_clients_[tx_audio_owner_.data()];
			const int prebuffer = state.txPrebufferMs < 0 ? default_tx_prebuffer_ms_ : state.txPrebufferMs;
			emit txAudioStarted(prebuffer);
		} else if (!requested && unkey_pending_) {
			unkey_pending_ = false;
			broadcastText(QStringLiteral("trx:0,false;"));
			broadcastText(txEnableMessage());
		}
	});

	tx_watchdog_.setSingleShot(true);
	tx_watchdog_.setTimerType(Qt::PreciseTimer);

	connect(&tx_watchdog_, &QTimer::timeout, this, &TciServer::onTxWatchdogExpired);

	// Poll the sample clock frequently; the sample count, not the timer
	// interval, determines when the next negotiated block is due.
	tx_chrono_timer_.setTimerType(Qt::PreciseTimer);
	tx_chrono_timer_.setInterval(2);
	connect(&tx_chrono_timer_, &QTimer::timeout, this, &TciServer::sendTxChrono);
}

TciServer::~TciServer()
{
	stopTransmit(QStringLiteral("server shutdown"), true);

	for (QWebSocket *client: std::as_const(clients_))
	{
		client->close();
		client->deleteLater();
	}

	clients_.clear();
	audio_clients_.clear();
	audio_stream_clients_.clear();
}

bool TciServer::listen(const QHostAddress &address, quint16 port)
{
	const bool ok = server_.listen(address, port);

	if (ok)
	{
		qInfo() << "TCI bridge listening on" << server_.serverAddress().toString() << server_.serverPort();
	}
	else
	{
		qCritical() << "Failed to listen:" << server_.errorString();
	}

	return ok;
}

void TciServer::onNewConnection()
{
	QWebSocket *socket = server_.nextPendingConnection();

	clients_.insert(socket);

	qInfo() << "TCI client connected:" << socket->peerAddress().toString() << socket->peerPort();

	connect(socket, &QWebSocket::textMessageReceived, this, &TciServer::onTextMessageReceived);

	connect(socket, &QWebSocket::binaryMessageReceived, this, &TciServer::onBinaryMessageReceived);

	connect(socket, &QWebSocket::disconnected, this, &TciServer::onSocketDisconnected);

	sendStartupBurst(socket);
	sendState(socket);
}

void TciServer::onTextMessageReceived(const QString &message)
{
	auto *socket = qobject_cast<QWebSocket *>(sender());

	if (!socket)
		return;

	if (debug_)
		qInfo().noquote() << "RX text:" << message;

	const TciCommand command = TciCommand::parse(message);

	if (!command.valid)
	{
		qWarning().noquote() << "Ignoring invalid TCI message:" << message;
		return;
	}

	handleCommand(socket, command);
}

void TciServer::onBinaryMessageReceived(const QByteArray &message)
{
	auto *socket = qobject_cast<QWebSocket *>(sender());

	if (!socket) {
		qWarning() << "Ignoring TCI binary frame with no sender socket";
		return;
	}

	const TciStream::StreamFrame frame = TciStream::parseFrame(message);

	if (!frame.valid) {
		qWarning() << "Invalid TCI binary frame:" << message.size() << "bytes";
		return;
	}

	if (frame.streamType != TciStream::TX_AUDIO_STREAM) {
		// Do not log every non-TX frame in normal operation.
		return;
	}

	if (!TciSampleRate::isSupported(frame.sampleRate)) {
		qWarning() << "Unsupported TCI TX sample rate:" << frame.sampleRate;
		return;
	}

	// Decode all uncompressed TCI sample types to the native mono int16
	// stream consumed by the existing Qt audio backend. Keep TX permission
	// and ownership checks below, unchanged.
	if (frame.receiver != 0 || frame.codec != 0 || frame.crc != 0 ||
	    frame.channels < 1 || frame.channels > 2) {
		qWarning() << "Unsupported TX audio receiver, codec, CRC or channels:"
		           << frame.receiver << frame.codec << frame.crc << frame.channels;
		return;
	}

	// An empty TX_AUDIO_STREAM packet is permitted to convey silence/no
	// samples. It must not touch ownership, watchdog or the audio sink.
	if (frame.sampleCount == 0 && frame.payload.isEmpty())
		return;

	if (!clientCanTransmit(socket, "TX audio frame"))
		return;

	// Only the client that keyed the radio may send TX audio.
	if (!tx_owner_ || tx_owner_ != socket) {
		qWarning() << "TX audio denied: client does not own TX";
		return;
	}

	// The current transmission must have selected TCI audio.
	if (!tx_audio_owner_ || tx_audio_owner_ != socket) {
		qWarning() << "TX audio denied: TCI audio is not selected";
		return;
	}

	if (!radio_->ptt() || !tx_chrono_timer_.isActive()) {
		qWarning() << "TX audio denied: radio is not transmitting";
		return;
	}

	const QByteArray monoInt16 = TciStream::convertTxToMonoInt16(frame);
	if (monoInt16.isEmpty()) {
		qWarning() << "Unable to decode TX audio frame:"
		           << frame.sampleType << frame.channels << frame.sampleCount;
		return;
	}

	// Process only valid frames from the current TX owner. Reconfigure
	// on a rate transition; each rate uses its own continuous FIR state.
	if (tx_input_sample_rate_ != frame.sampleRate) {
		if (!tx_interpolator_.configure(frame.sampleRate))
			return;
		tx_input_sample_rate_ = frame.sampleRate;
	}
	const QByteArray output48k = tx_interpolator_.process(monoInt16);
	if (output48k.isEmpty())
		return;

	logTxFrameTiming(frame);

	emit txAudioFrame(output48k);
}

void TciServer::onSocketDisconnected()
{
	auto *socket = qobject_cast<QWebSocket *>(sender());

	if (!socket)
		return;

	clients_.remove(socket);
	if (rx_meter_clients_.remove(socket))
		updateRxMeterInterval();
	if (tx_meter_clients_.remove(socket))
		updateTxMeterInterval();
	audio_clients_.remove(socket);
	audio_stream_clients_.remove(socket);

	qInfo() << "TCI client disconnected:" << socket->peerAddress().toString() << socket->peerPort();

	if (tx_audio_owner_ == socket)
	{
		stopTxAudioStreamForSocket(socket, QStringLiteral("TX audio owner disconnected"));
	}

	if (tx_owner_ == socket) {
		qWarning() << "TX owner disconnected";

		if (unkey_on_disconnect_)
			stopTransmit(QStringLiteral("TX owner disconnected"), true);
		else
			tx_owner_.clear();
	}

	socket->deleteLater();
}

void TciServer::updateTxMeterInterval()
{
	if (!allow_partial_tx_sensors_ && !radio_->supportsCompleteTxMeter())
		return;

	int interval = 0;
	for (const auto &subscription : std::as_const(tx_meter_clients_)) {
		if (!interval || subscription.intervalMs < interval)
			interval = subscription.intervalMs;
	}
	if (interval != tx_meter_interval_ms_) {
		tx_meter_interval_ms_ = interval;
		radio_->setTxMeterInterval(interval);
	}
}

void TciServer::sendTxMeterReading(const TxMeterReading &reading)
{
	if (!radio_->online() || !radio_->ptt() || unkey_pending_ || tx_meter_clients_.isEmpty())
		return;

	const auto valid = [](std::optional<double> value) {
		return value && std::isfinite(*value);
	};
	const bool microphone = valid(reading.microphoneDbm);
	const bool output = valid(reading.outputWatts) && *reading.outputWatts >= 0;
	const bool peak = valid(reading.peakWatts) && *reading.peakWatts >= 0;
	const bool swr = valid(reading.swr) && *reading.swr >= 1;
	if (!(microphone || output || peak || swr) ||
	    (!allow_partial_tx_sensors_ && !(microphone && output && peak && swr)))
		return;

	const auto format = [](std::optional<double> value, bool known) {
		return known ? QString::number(*value, 'g', 8) : QStringLiteral("nan");
	};
	const QString message = QStringLiteral("tx_sensors:0,%1,%2,%3,%4;")
		.arg(format(reading.microphoneDbm, microphone), format(reading.outputWatts, output),
		     format(reading.peakWatts, peak), format(reading.swr, swr));
	for (auto it = tx_meter_clients_.begin(); it != tx_meter_clients_.end(); ++it) {
		if (!it->lastSent.isValid() || it->lastSent.elapsed() >= it->intervalMs) {
			it->lastSent.start();
			sendText(it.key(), message);
		}
	}
}

void TciServer::updateRxMeterInterval()
{
	int interval = 0;
	for (const auto &subscription : std::as_const(rx_meter_clients_)) {
		if (!interval || subscription.intervalMs < interval)
			interval = subscription.intervalMs;
	}
	if (interval != rx_meter_interval_ms_) {
		rx_meter_interval_ms_ = interval;
		radio_->setRxMeterInterval(interval);
	}
}

void TciServer::sendRxSignalLevel(int dbm)
{
	if (!radio_->online() || transmitActive())
		return;
	for (auto it = rx_meter_clients_.begin(); it != rx_meter_clients_.end(); ++it) {
		if (it->lastDbm == dbm ||
		    (it->lastSent.isValid() && it->lastSent.elapsed() < it->intervalMs))
			continue;
		it->lastDbm = dbm;
		it->lastSent.start();
		sendText(it.key(), QStringLiteral("rx_sensors:0,%1;").arg(dbm));
		sendText(it.key(), QStringLiteral("rx_channel_sensors:0,0,%1;").arg(dbm));
	}
}

void TciServer::onFrequencyChanged(qint64 hz)
{
	broadcastText(QStringLiteral("vfo:0,0,%1;").arg(hz));
	broadcastText(QStringLiteral("dds:0,%1;").arg(hz));
	broadcastTxFrequency();
}

void TciServer::onTxFrequencyChanged(qint64 hz)
{
	if (hz > 0)
		broadcastText(QStringLiteral("vfo:0,1,%1;").arg(hz));
	broadcastTxFrequency();
}

void TciServer::onSplitChanged(bool enabled)
{
	broadcastText(QStringLiteral("split_enable:0,%1;").arg(boolText(enabled)));
	broadcastTxFrequency();
}

void TciServer::onModeChanged(const QString &mode)
{
	broadcastText(QStringLiteral("modulation:0,%1;").arg(mode.toLower()));
}

void TciServer::onPttChanged(bool enabled)
{
	for (auto &subscription : tx_meter_clients_)
		subscription.lastSent.invalidate();
	if (enabled || !unkey_pending_)
		broadcastText(QStringLiteral("trx:0,%1;").arg(boolText(enabled)));
	for (auto &state : audio_clients_) {
		state.packetCount = 0;
		state.accumulator.clear();
		state.decimator.reset();
	}
	tx_chrono_packet_count_ = 0;

	if (!enabled) {
		stopTxAudioStream(QStringLiteral("radio returned to RX"));
		disarmTxWatchdog();
		tx_owner_.clear();
	}
}

void TciServer::onRadioOnlineChanged(bool online)
{
	qInfo() << "Radio online state changed:" << online;
	if (!online) {
		last_tx_frequency_ = 0;
		for (auto &subscription : rx_meter_clients_) {
			subscription.lastDbm.reset();
			subscription.lastSent.invalidate();
		}
		for (auto &subscription : tx_meter_clients_)
			subscription.lastSent.invalidate();
		broadcastText(txEnableMessage());
		if (transmitActive())
			stopTransmit(QStringLiteral("radio went offline"), true);
		return;
	}

	if (unkey_pending_)
		radio_->setPtt(false);

	for (QWebSocket *client : std::as_const(clients_))
		sendState(client);
}

void TciServer::txAudioOutputReady()
{
	if (!tx_audio_owner_ || tx_audio_owner_ != tx_owner_ ||
	    unkey_pending_ || !radio_->ptt())
		return;

	tx_chrono_samples_requested_ = 0;
	tx_chrono_clock_.start();
	tx_chrono_timer_.start();
	sendTxChrono();
}

void TciServer::sendStartupBurst(QWebSocket *socket)
{
	// Radio-specific vfo_limits, if_limits, and modulations_list cannot
	// safely be advertised until the backend provides verified capabilities.
	for (const QString &message : TciStartup::messages(transmit_enabled_)) {
		if (message == QStringLiteral("ready;") && radio_) {
			const auto limits = radio_->vfoLimits();
			if (limits.first > 0 && limits.second > limits.first)
				sendText(socket, QStringLiteral("vfo_limits:%1,%2;")
				                 .arg(limits.first).arg(limits.second));
		}
		if (message == QStringLiteral("ready;") && radio_ &&
		    !radio_->supportedModes().isEmpty()) {
			sendText(socket, QStringLiteral("modulations_list:%1;")
			                 .arg(radio_->supportedModes().join(',')));
		}
		sendText(socket, message);
	}
}

void TciServer::setTransmitEnabled(bool enabled)
{
	transmit_enabled_ = enabled;

	qInfo() << "Transmit control" << (transmit_enabled_ ? "ENABLED" : "DISABLED");

	// The permission may change while a client is connected. Keep the
	// advertised receive-only state synchronized with the actual policy.
	if (server_.isListening()) {
		broadcastText(QStringLiteral("receive_only:%1;").arg(boolText(!transmit_enabled_)));
		broadcastText(txEnableMessage());
	}

	if (!transmit_enabled_ && transmitActive())
		stopTransmit(QStringLiteral("transmit disabled"), true);
}

bool TciServer::transmitActive() const
{
	return tx_owner_ || tx_audio_owner_ || tx_watchdog_.isActive() ||
	       unkey_pending_ || (radio_ && radio_->ptt());
}

void TciServer::setMaxTxMs(int maxTxMs)
{
	max_tx_ms_ = qMax(1000, maxTxMs);

	qInfo() << "Max TX duration:" << max_tx_ms_ << "ms";
}

void TciServer::setUnkeyOnDisconnect(bool enabled)
{
	unkey_on_disconnect_ = enabled;

	qInfo() << "Unkey on disconnect" << (unkey_on_disconnect_ ? "ENABLED" : "DISABLED");
}

void TciServer::setDebug(bool enabled)
{
	debug_ = enabled;

	qInfo() << "TCI protocol debug" << (debug_ ? "ENABLED" : "DISABLED");
}

void TciServer::setQuiet(bool enabled)
{
	quiet_ = enabled;
}

void TciServer::forceUnkey(const QString &reason)
{
	stopTransmit(reason, true);
}

bool TciServer::requestPtt(QWebSocket *socket,
						   bool enabled,
						   const QString &reason)
{
	if (enabled) {
		if (!transmit_enabled_ || !radio_->online() || unkey_pending_) {
			qWarning().noquote()
				<< "PTT request denied while TX is disabled, offline or awaiting unkey:"
				<< reason;

			if (socket)
				sendText(socket, QStringLiteral("trx:0,%1;")
							 .arg(boolText(radio_ && radio_->ptt())));

			return false;
		}

		if (tx_owner_ && tx_owner_ != socket) {
			qWarning().noquote()
				<< "PTT request denied because another client owns TX:"
				<< reason;

			if (socket)
				sendText(socket, QStringLiteral("trx:0,%1;")
							 .arg(boolText(radio_ && radio_->ptt())));

			return false;
		}

		if (radio_->ptt()) {
			sendText(socket, QStringLiteral("trx:0,true;"));
			return false;
		}

		tx_owner_ = socket;

		qInfo().noquote() << "PTT ON:" << reason;

		armTxWatchdog(reason);
		radio_->setPtt(true);

		return true;
	}

	// Never allow a different client to release another client's TX.
	if (tx_owner_ && tx_owner_ != socket) {
		qWarning().noquote()
			<< "PTT OFF denied because another client owns TX:"
			<< reason;
		return false;
	}

	qInfo().noquote() << "PTT OFF:" << reason;

	stopTransmit(reason, true);
	return true;
}

bool TciServer::clientCanTransmit(QWebSocket *socket, const QString &reason) const
{
	Q_UNUSED(socket)

	if (!transmit_enabled_) {
		qWarning().noquote()
			<< "TX denied because transmit is disabled:"
			<< reason;
		return false;
	}

	return true;
}

void TciServer::armTxWatchdog(const QString &reason)
{
	if (max_tx_ms_ <= 0)
		return;

	if (tx_watchdog_.isActive())
		return;

	tx_watchdog_.start(max_tx_ms_);

	qInfo() << "TX watchdog armed:" << max_tx_ms_ << "ms reason=" << reason;
}

void TciServer::disarmTxWatchdog()
{
	if (tx_watchdog_.isActive())
		tx_watchdog_.stop();
}

void TciServer::onTxWatchdogExpired()
{
	qCritical() << "TX watchdog expired after"
				<< max_tx_ms_
				<< "ms";

	stopTransmit(QStringLiteral("TX watchdog expired"), true);
}

QString TciServer::txEnableMessage() const
{
	return QStringLiteral("tx_enable:0,%1;")
		.arg(boolText(transmit_enabled_ && radio_->online() && !unkey_pending_));
}

void TciServer::broadcastTxFrequency()
{
	const qint64 hz = radio_->effectiveTxFrequencyHz();
	if (last_tx_frequency_ == hz)
		return;
	last_tx_frequency_ = hz;
	if (hz > 0)
		broadcastText(QStringLiteral("tx_frequency:%1;").arg(hz));
}

void TciServer::sendState(QWebSocket *socket)
{
	if (!radio_)
		return;

	sendText(socket, QStringLiteral("vfo:0,0,%1;").arg(radio_->frequencyHz()));
	sendText(socket, QStringLiteral("vfo:0,1,%1;").arg(radio_->txFrequencyHz()));
	sendText(socket, QStringLiteral("dds:0,%1;").arg(radio_->frequencyHz()));
	sendText(socket, QStringLiteral("modulation:0,%1;").arg(radio_->mode().toLower()));
	sendText(socket, QStringLiteral("split_enable:0,%1;").arg(boolText(radio_->splitEnabled())));
	sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
	sendText(socket, txEnableMessage());
	for (size_t i = 0; i < rxControlNames.size(); ++i) {
		const auto setting = static_cast<RxControl>(i);
		if (const auto value = radio_->rxControlValue(setting))
			sendText(socket, rxControlMessage(setting, *value));
	}
	const auto [low, high] = radio_->rxFilterBand();
	if (low < high)
		sendText(socket, QStringLiteral("rx_filter_band:0,%1,%2;").arg(low).arg(high));
	const QString agc = radio_->agcMode();
	if (!agc.isEmpty())
		sendText(socket, QStringLiteral("agc_mode:0,%1;").arg(agc));
	const int drive = radio_->drivePercent();
	if (drive >= 0)
		sendText(socket, QStringLiteral("drive:0,%1;").arg(drive));
	const qint64 txHz = radio_->effectiveTxFrequencyHz();
	if (txHz > 0)
		sendText(socket, QStringLiteral("tx_frequency:%1;").arg(txHz));
}

void TciServer::handleCommand(QWebSocket *socket, const TciCommand &command)
{
	const QString &name = command.name;
	const QStringList &a = command.args;

	for (size_t i = 0; i < rxControlNames.size(); ++i) {
		if (name != QLatin1String(rxControlNames[i]))
			continue;
		if (a.isEmpty() || a.at(0) != "0" || a.size() > 2)
			return;

		const auto setting = static_cast<RxControl>(i);
		if (a.size() == 1) {
			if (!transmitActive() &&
			    (setting == RxControl::NoiseBlanker || setting == RxControl::NoiseReduction)) {
				radio_->requestRxControl(setting);
				return;
			}
			if (const auto value = radio_->rxControlValue(setting))
				sendText(socket, rxControlMessage(setting, *value));
			return;
		}

		if (transmitActive())
			return;
		if (rxControlIsSwitch(setting)) {
			const QString value = a.at(1).toLower();
			if (value == "true" || value == "false")
				radio_->setRxControl(setting, value == "true");
		} else {
			bool ok = false;
			const int value = a.at(1).toInt(&ok);
			if (ok && value != std::numeric_limits<int>::min())
				radio_->setRxControl(setting, value);
		}
		return;
	}

	if (name == "rx_sensors_enable" || name == "tx_sensors_enable") {
		if (a.isEmpty() || a.size() > 2)
			return;
		const QString enable = a.at(0).toLower();
		if (enable != "true" && enable != "false")
			return;

		int interval = 250;
		if (a.size() == 2) {
			bool ok = false;
			interval = a.at(1).toInt(&ok);
			if (!ok || interval < 30 || interval > 1000)
				return;
		}

		if (name == "tx_sensors_enable") {
			if (enable == "true")
				tx_meter_clients_.insert(socket, TxMeterSubscription{interval, {}});
			else
				tx_meter_clients_.remove(socket);
			updateTxMeterInterval();
			return;
		}

		if (enable == "true")
			rx_meter_clients_.insert(socket, RxMeterSubscription{interval, {}, {}});
		else
			rx_meter_clients_.remove(socket);
		updateRxMeterInterval();
		return;
	}

	if (name == "rx_filter_band") {
		if (a.isEmpty() || a.at(0) != "0" || transmitActive())
			return;
		if (a.size() == 1)
			radio_->requestRxFilterBand();
		else if (a.size() == 3) {
			bool lowOk = false;
			bool highOk = false;
			const int low = a.at(1).toInt(&lowOk);
			const int high = a.at(2).toInt(&highOk);
			if (lowOk && highOk && low < high)
				radio_->setRxFilterBand(low, high);
		}
		return;
	}

	if (name == "agc_mode") {
		if (a.isEmpty() || a.at(0) != "0" || a.size() > 2)
			return;
		if (a.size() == 1) {
			if (!transmitActive())
				radio_->requestAgcReadback();
			else if (const QString mode = radio_->agcMode(); !mode.isEmpty())
				sendText(socket, QStringLiteral("agc_mode:0,%1;").arg(mode));
		} else if (!transmitActive()) {
			const QString mode = a.at(1).toLower();
			if (mode == "normal" || mode == "fast" || mode == "off")
				radio_->setAgcMode(mode);
		}
		return;
	}

	if (name == "drive") {
		if (a.isEmpty() || a.at(0) != "0" || a.size() > 2)
			return;
		if (a.size() == 1) {
			if (!transmitActive()) {
				radio_->requestDriveReadback();
				return;
			}
			const int drive = radio_->drivePercent();
			if (drive >= 0)
				sendText(socket, QStringLiteral("drive:0,%1;").arg(drive));
		} else if (!transmitActive()) {
			bool ok = false;
			const int drive = a.at(1).toInt(&ok);
			if (ok && drive >= 0 && drive <= 100)
				radio_->setDrivePercent(drive);
		} else {
			qWarning() << "DRIVE write ignored while transmit is active";
		}
		return;
	}

	// Clients may request an initial state refresh after connecting.
	// START is not an acknowledgement of a physical radio power-on.
	if (name == "start") {
		sendState(socket);
		return;
	}

	// TCI 2.1 DDS addresses a receiver, not a VFO channel:
	//   dds:<receiver>;           read
	//   dds:<receiver>,<hz>;      set
	// A rigctld radio does not expose an independent SDR panorama
	// center frequency, so DDS is mapped to the RX frequency.
	if (name == "dds") {
		if (a.size() < 1 || a.size() > 2 || a.at(0) != "0")
			return;

		if (a.size() == 2) {
			bool ok = false;
			const qint64 hz = a.at(1).toLongLong(&ok);
			if (!ok || hz <= 0)
				return;

			radio_->setFrequencyHz(hz);
			return;
		}

		sendText(socket, QStringLiteral("dds:0,%1;").arg(radio_->frequencyHz()));
		return;
	}

	// TCI 2.1 VFO includes both receiver and channel:
	//   vfo:<receiver>,<channel>;         read
	//   vfo:<receiver>,<channel>,<hz>;    set
	// Channel 0 is RX; channel 1 maps to the existing best-effort TX
	// frequency control/shadow in the radio backend.
	if (name == "vfo") {
		if (a.size() < 2 || a.size() > 3 || a.at(0) != "0")
			return;

		bool channelOk = false;
		const int channel = a.at(1).toInt(&channelOk);
		if (!channelOk || (channel != 0 && channel != 1))
			return;

		if (a.size() == 3) {
			bool ok = false;
			const qint64 hz = a.at(2).toLongLong(&ok);
			if (!ok || hz <= 0)
				return;

			if (channel == 0)
				radio_->setFrequencyHz(hz);
			else
				radio_->setTxFrequencyHz(hz);
			return;
		}

		const qint64 hz = channel == 0
			? radio_->frequencyHz()
			: radio_->txFrequencyHz();
		sendText(socket, QStringLiteral("vfo:0,%1,%2;").arg(channel).arg(hz));
		return;
	}

	// TCI 2.1 MODULATION addresses a receiver, not a VFO channel:
	//   modulation:<receiver>;           read
	//   modulation:<receiver>,<mode>;    set
	if (name == "modulation") {
		if (a.size() < 1 || a.size() > 2 || a.at(0) != "0")
			return;

		if (a.size() == 2) {
			const QString mode = a.at(1).trimmed();
			if (mode.isEmpty())
				return;

			radio_->setMode(mode);
			return;
		}

		sendText(socket, QStringLiteral("modulation:0,%1;")
			.arg(radio_->mode().toLower()));
		return;
	}

	if (name == "tx_enable") {
		if (!a.isEmpty() && a.at(0) == "0" && a.size() <= 2)
			sendText(socket, txEnableMessage());
		return;
	}

	if (name == "trx" || name == "ptt") {
		if (a.size() > 3 || (!a.isEmpty() && a.at(0) != "0"))
			return;
		if (a.size() <= 1) {
			sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
			return;
		}

		const QString value = a.at(1).toLower();
		const bool enabled = value == "true" || value == "1" || value == "on" || value == "tx";
		if (!enabled && value != "false" && value != "0" && value != "off" && value != "rx")
			return;
		const QString source = a.value(2).toLower();
		if (a.size() == 3 && source != "tci")
			return;

		if (!enabled) {
			requestPtt(socket, false, command.raw);
			return;
		}

		if (tx_owner_ == socket) {
			sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
			return;
		}
		// Install the audio owner before calling setPtt: the null backend
		// completes synchronously.
		if (source == "tci" && !tx_owner_)
			tx_audio_owner_ = socket;
		if (!requestPtt(socket, true, command.raw) && tx_audio_owner_ == socket)
			tx_audio_owner_.clear();
		return;
	}

	if (name == "split_enable") {
		if (a.isEmpty() || a.at(0) != "0" || a.size() > 2)
			return;
		if (a.size() == 2) {
			const QString value = a.at(1).toLower();
			if (value != "true" && value != "false")
				return;
			radio_->setSplitEnabled(value == "true");
			return;
		}
		sendText(socket, QStringLiteral("split_enable:0,%1;").arg(boolText(radio_->splitEnabled())));
		return;
	}

	// STOP means stop the device in TCI, not close the WebSocket.
	// This bridge has no corresponding radio power-control operation.
	if (name == "stop")
		return;

	if (name == "close") {
		socket->close();
		return;
	}

	if (name == "audio_start")
	{
		if (a.size() != 1 || a.at(0) != "0")
			return;
		if (!audio_clients_.contains(socket))
			audio_clients_.insert(socket, AudioClientState{});
		audio_clients_[socket].accumulator.clear();
		audio_clients_[socket].decimator.reset();
		audio_stream_clients_.insert(socket);
		qInfo() << "Enabled RX audio stream for client:" << socket->peerAddress().toString() << socket->peerPort();
		return;
	}

	if (name == "audio_stop")
	{
		if (a.size() != 1 || a.at(0) != "0")
			return;
		audio_stream_clients_.remove(socket);
		if (audio_clients_.contains(socket)) {
			audio_clients_[socket].accumulator.clear();
			audio_clients_[socket].decimator.reset();
		}
		qInfo() << "Disabled RX audio stream for client:" << socket->peerAddress().toString() << socket->peerPort();
		return;
	}

	if (name == "audio_samplerate" ||
	    name == "audio_stream_sample_type" ||
	    name == "audio_stream_channels" ||
	    name == "audio_stream_samples" ||
	    name == "tx_stream_audio_buffering")
	{
		// Audio settings belong to the requesting connection, even before
		// audio_start. A second client must never change another's stream.
		AudioClientState &state = audio_clients_[socket];
		const bool allowSet = a.size() == 1 && tx_audio_owner_ != socket;

		if (name == "tx_stream_audio_buffering") {
			if (allowSet) {
				bool ok = false;
				const int requested = a.at(0).toInt(&ok);
				if (ok)
					state.txPrebufferMs = qBound(50, requested, 500);
			}
			const int current = state.txPrebufferMs < 0 ? default_tx_prebuffer_ms_ : state.txPrebufferMs;
			sendText(socket, QStringLiteral("tx_stream_audio_buffering:%1;").arg(current));
			return;
		}

		if (name == "audio_samplerate") {
			// The current Qt audio backend delivers 48-kHz PCM. Do not
			// claim sample rates that are not actually resampled.
			if (allowSet) {
				bool ok = false;
				const quint32 requested = a.at(0).toUInt(&ok);
				if (ok && TciSampleRate::isSupported(requested) &&
				    state.decimator.configure(requested)) {
					state.sampleRate = requested;
					if (!state.samplesExplicit) {
						switch (requested) {
						case 8000: state.samplesPerFrame = 256; break;
						case 12000: state.samplesPerFrame = 512; break;
						case 24000: state.samplesPerFrame = 1024; break;
						default: state.samplesPerFrame = TciStream::DEFAULT_RX_SAMPLES_PER_FRAME; break;
						}
					}
					state.accumulator.clear();
				}
			}
			sendText(socket, QStringLiteral("audio_samplerate:%1;").arg(state.sampleRate));
			return;
		}

		if (name == "audio_stream_sample_type") {
			if (allowSet) {
				const QString requested = a.at(0).trimmed().toLower();
				if (requested == "int16") state.sampleType = TciStream::INT16;
				else if (requested == "int24") state.sampleType = TciStream::INT24;
				else if (requested == "int32") state.sampleType = TciStream::INT32;
				else if (requested == "float32") state.sampleType = TciStream::FLOAT32;
				state.accumulator.clear();
			}
			QString current = "float32";
			if (state.sampleType == TciStream::INT16) current = "int16";
			else if (state.sampleType == TciStream::INT24) current = "int24";
			else if (state.sampleType == TciStream::INT32) current = "int32";
			else if (state.sampleType == TciStream::FLOAT32) current = "float32";
			sendText(socket, QStringLiteral("audio_stream_sample_type:%1;").arg(current));
			return;
		}

		if (name == "audio_stream_channels") {
			if (allowSet) {
				bool ok = false;
				const int requested = a.at(0).toInt(&ok);
				if (ok) {
					state.channels = static_cast<quint32>(qBound(1, requested, 2));
					if (state.channels == 2 && (state.samplesPerFrame & 1U))
						--state.samplesPerFrame;
					state.accumulator.clear();
				}
			}
			sendText(socket, QStringLiteral("audio_stream_channels:%1;").arg(state.channels));
			return;
		}

		if (name == "audio_stream_samples") {
			if (allowSet) {
				bool ok = false;
				const int requested = a.at(0).toInt(&ok);
				if (ok) {
					state.samplesExplicit = true;
					state.samplesPerFrame = static_cast<quint32>(qBound(100, requested, 2048));
					if (state.channels == 2 && (state.samplesPerFrame & 1U))
						--state.samplesPerFrame;
					state.accumulator.clear();
				}
			}
			sendText(socket, QStringLiteral("audio_stream_samples:%1;").arg(state.samplesPerFrame));
			return;
		}
	}


	if (!warned_unknown_commands_.contains(name)) {
		warned_unknown_commands_.insert(name);

		qWarning().noquote()
			<< "Unsupported TCI command:"
			<< command.raw;
	}

	return;
}

void TciServer::sendText(QWebSocket *socket, const QString &message)
{
	if (!socket)
		return;

	const QString framed = semicolon(message);

	if (debug_)
		qInfo().noquote() << "TX text:" << framed;

	socket->sendTextMessage(framed);
}

void TciServer::sendRxAudio(const QByteArray &pcm, int sampleRate, int channels)
{
	if (audio_stream_clients_.isEmpty() || pcm.isEmpty())
		return;

	// The Qt audio backend currently supplies 48-kHz mono signed int16.
	// Never mislabel source bytes as a requested but unsupported format.
	if (sampleRate != 48000 || channels != 1 || (pcm.size() % 2) != 0) {
		qWarning() << "Unsupported source RX PCM format:" << sampleRate
		           << "Hz," << channels << "channels, bytes=" << pcm.size();
		return;
	}

	for (auto it = audio_clients_.begin(); it != audio_clients_.end(); ++it) {
		if (!audio_stream_clients_.contains(it.key()))
			continue;
		AudioClientState &state = it.value();
		const QByteArray atRequestedRate = state.decimator.process(pcm);
		state.accumulator.append(atRequestedRate);
		// Stream.length counts individual sample values across channels.
		// The mono source must therefore contribute N/channels time samples.
		const int monoSamples = static_cast<int>(state.samplesPerFrame / state.channels);
		const int sourceBytes = monoSamples * 2;
		if (sourceBytes <= 0)
			continue;

		while (state.accumulator.size() >= sourceBytes) {
			const QByteArray sourceChunk = state.accumulator.left(sourceBytes);
			state.accumulator.remove(0, sourceBytes);
			const QByteArray encoded = TciStream::convertRxMonoInt16(
				sourceChunk, state.sampleType, state.channels);
			if (encoded.isEmpty())
				continue;
			const QByteArray frame = TciStream::makeRxAudioFrame(
				encoded, 0, state.sampleRate, state.sampleType,
				state.channels, state.packetCount++);
			it.key()->sendBinaryMessage(frame);
		}
	}
}

void TciServer::sendTxChrono()
{
	if (!tx_audio_owner_ || !tx_owner_ ||
	    tx_audio_owner_ != tx_owner_ || !radio_ || !radio_->ptt()) {
		tx_chrono_timer_.stop();
		return;
	}

	const auto &format = audio_clients_[tx_audio_owner_.data()];
	const quint64 sampleRate = format.sampleRate;
	const quint32 samplesPerFrame = format.samplesPerFrame;
	const quint32 framesPerPacket = samplesPerFrame / format.channels;
	// A timer callback can be late on a loaded Pi. Drop overdue requests
	// instead of sending a burst that would flood the TX audio queue.
	const quint64 samplesElapsed =
		static_cast<quint64>(tx_chrono_clock_.nsecsElapsed() / 1000) * sampleRate / 1000000ULL;
	if (tx_chrono_samples_requested_ > samplesElapsed)
		return;

	const QByteArray frame = TciStream::makeTxChronoFrame(
		0, format.sampleRate, format.sampleType,
		format.channels, samplesPerFrame, tx_chrono_packet_count_++);
	tx_audio_owner_->sendBinaryMessage(frame);
	tx_chrono_samples_requested_ = (samplesElapsed / framesPerPacket + 1) * framesPerPacket;
}

void TciServer::stopTxAudioStream(const QString &reason)
{
	if (!tx_audio_owner_)
		return;

	qInfo() << "TX audio stream stopped:"
			<< reason;

	tx_chrono_timer_.stop();
	tx_chrono_samples_requested_ = 0;
	tx_audio_owner_ = nullptr;
	for (auto it = audio_clients_.begin(); it != audio_clients_.end(); ++it) {
		it.value().packetCount = 0;
		it.value().accumulator.clear();
	}
	tx_chrono_packet_count_ = 0;
	tx_interpolator_.configure(48000);
	tx_input_sample_rate_ = 48000;

	emit txAudioStopped();

	if (radio_)
		radio_->setPollingSuspended(false);
}

void TciServer::stopTxAudioStreamForSocket(QWebSocket *socket, const QString &reason)
{
	if (!tx_audio_owner_)
		return;

	if (tx_audio_owner_ != socket) {
		qInfo() << "TX audio stop from non-owner ignored:"
				<< reason;
		return;
	}

	stopTxAudioStream(reason);
}

void TciServer::stopTransmit(const QString &reason, bool forcePttOff)
{
	stopTxAudioStream(reason);
	disarmTxWatchdog();
	tx_owner_.clear();

	if (radio_) {
		radio_->setPollingSuspended(false);
	}

	if (!radio_) return;

	if ((forcePttOff || radio_->ptt()) && !unkey_pending_) {
		qWarning().noquote() << "Force unkey:" << reason;
		unkey_pending_ = true;
		broadcastText(txEnableMessage());

		if (!radio_->online()) {
			qWarning() << "Radio is offline; will force PTT off when radio comes online";
			return;
		}

		radio_->setPtt(false);
	}
}

void TciServer::logTxFrameTiming(const TciStream::StreamFrame &frame)
{
	if (!tx_timing_log_enabled_)
		return;

	static QElapsedTimer frameTimer;
	static bool timerStarted = false;
	static qint64 lastNs = 0;
	static qint64 frames = 0;
	static qint64 windowFrames = 0;
	static qint64 minUs = std::numeric_limits<qint64>::max();
	static qint64 maxUs = 0;
	static qint64 sumUs = 0;

	if (!timerStarted) {
		frameTimer.start();
		timerStarted = true;
		lastNs = frameTimer.nsecsElapsed();
	}

	const qint64 nowNs = frameTimer.nsecsElapsed();
	const qint64 deltaUs = (nowNs - lastNs) / 1000;
	lastNs = nowNs;

	if (frames > 0) {
		minUs = qMin(minUs, deltaUs);
		maxUs = qMax(maxUs, deltaUs);
		sumUs += deltaUs;
		++windowFrames;
	}

	++frames;

	if (windowFrames >= 500) {
		if (!quiet_)
		{
			qInfo() << "Bridge TX frame timing"
					<< "frames =" << frames
					<< "min_us =" << minUs
					<< "avg_us =" << (sumUs / qMax<qint64>(1, windowFrames))
					<< "max_us =" << maxUs
					<< "payload =" << frame.payload.size();
		}

		windowFrames = 0;
		minUs = std::numeric_limits<qint64>::max();
		maxUs = 0;
		sumUs = 0;
	}
}

void TciServer::broadcastText(const QString &message)
{
	const QString framed = semicolon(message);

	if (debug_)
		qInfo().noquote() << "BCAST text:" << framed;

	for (QWebSocket *client: std::as_const(clients_))
		client->sendTextMessage(framed);
}

void TciServer::setTxTimingLogEnabled(bool enabled)
{
	tx_timing_log_enabled_ = enabled;
}

QString TciServer::boolText(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }

QString TciServer::semicolon(QString message)
{
	message = message.trimmed();

	if (!message.endsWith(';'))
		message.append(';');

	return message;
}