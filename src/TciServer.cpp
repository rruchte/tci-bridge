#include "TciServer.h"

#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>
#include <cstring>
#include <limits>

TciServer::TciServer(RadioBackend *radio, QObject *parent) :
	QObject(parent), server_(QStringLiteral("tci-bridge"), QWebSocketServer::NonSecureMode, this), radio_(radio)
{
	connect(&server_, &QWebSocketServer::newConnection, this, &TciServer::onNewConnection);

	connect(radio_, &RadioBackend::frequencyChanged, this, &TciServer::onFrequencyChanged);

	connect(radio_, &RadioBackend::txFrequencyChanged,this, &TciServer::onTxFrequencyChanged);

	connect(radio_, &RadioBackend::splitChanged, this, &TciServer::onSplitChanged);

	connect(radio_, &RadioBackend::modeChanged, this, &TciServer::onModeChanged);

	connect(radio_, &RadioBackend::pttChanged, this, &TciServer::onPttChanged);

	connect(radio_, &RadioBackend::onlineChanged, this, &TciServer::onRadioOnlineChanged);

	tx_watchdog_.setSingleShot(true);

	connect(&tx_watchdog_, &QTimer::timeout, this, &TciServer::onTxWatchdogExpired);

	// Poll the sample clock frequently; the sample count, not the timer
	// interval, determines when the next 512-sample request is due.
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

	if (frame.sampleRate != 48000) {
		qWarning() << "Only 48000 Hz TX audio is currently supported; got sampleRate="
				   << frame.sampleRate;
		return;
	}

	// Decode all uncompressed TCI sample types to the native mono int16
	// stream consumed by the existing Qt audio backend. Keep TX permission
	// and ownership checks below, unchanged.
	if (frame.codec != 0 || frame.channels < 1 || frame.channels > 2) {
		qWarning() << "Unsupported TX audio codec/channel configuration:"
		           << frame.codec << frame.channels;
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

	if (!radio_->ptt()) {
		qWarning() << "TX audio denied: radio is not transmitting";
		return;
	}

	const QByteArray monoInt16 = TciStream::convertTxToMonoInt16(frame);
	if (monoInt16.isEmpty()) {
		qWarning() << "Unable to decode TX audio frame:"
		           << frame.sampleType << frame.channels << frame.sampleCount;
		return;
	}

	armTxWatchdog("TX audio frame");

	logTxFrameTiming(frame);

	emit txAudioFrame(monoInt16);
}

void TciServer::onSocketDisconnected()
{
	auto *socket = qobject_cast<QWebSocket *>(sender());

	if (!socket)
		return;

	clients_.remove(socket);
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

void TciServer::onFrequencyChanged(qint64 hz)
{
	broadcastText(QStringLiteral("vfo:0,0,%1;").arg(hz));
	broadcastText(QStringLiteral("dds:0,%1;").arg(hz));
}

void TciServer::onTxFrequencyChanged(qint64 hz)
{
	broadcastText(QStringLiteral("vfo:0,1,%1;").arg(hz));
}

void TciServer::onSplitChanged(bool enabled)
{
	broadcastText(QStringLiteral("split_enable:0,%1;").arg(boolText(enabled)));
}

void TciServer::onModeChanged(const QString &mode)
{
	broadcastText(QStringLiteral("modulation:0,%1;").arg(mode.toLower()));
}

void TciServer::onPttChanged(bool enabled)
{
	broadcastText(QStringLiteral("trx:0,%1;").arg(boolText(enabled)));
}

void TciServer::onRadioOnlineChanged(bool online)
{
	qInfo() << "Radio online state changed:" << online;

	if (!online) {
		if (transmitActive()) {
			qWarning() << "Radio went offline during active TX/audio; aborting TX and scheduling unkey";
			pending_unkey_on_radio_online_ = true;
			stopTransmit(QStringLiteral("radio went offline"), true);
		}

		return;
	}

	if (pending_unkey_on_radio_online_) {
		qWarning() << "Radio came online; forcing pending PTT off";

		pending_unkey_on_radio_online_ = false;

		stopTxAudioStream(QStringLiteral("pending unkey after radio online"));
		disarmTxWatchdog();
		tx_owner_.clear();

		if (radio_) {
			radio_->setPollingSuspended(false);
			radio_->setPtt(false);
		}

		broadcastText(QStringLiteral("trx:0,false;"));
	}

	// Push fresh state to existing clients after recovery.
	for (QWebSocket *client : std::as_const(clients_)) {
		sendState(client);
	}
}

void TciServer::sendStartupBurst(QWebSocket *socket)
{
	sendText(socket, "protocol:tci-bridge,2.1;");
	sendText(socket, "device:tci-bridge;");
	sendText(socket, "receive_only:false;");
	sendText(socket, "trx_count:1;");
	sendText(socket, "channels_count:1;");

	// TODO Replace with actual supported limits if known.
	sendText(socket, "vfo_limits:100000,60000000;");
	sendText(socket, "if_limits:-48000,48000;");

	// TODO Must reflect the actual radio's supported modes.
	sendText(socket, "modulations_list:AM,LSB,USB,CW,FM;");

	sendText(socket, "ready;");
}

void TciServer::setTransmitEnabled(bool enabled)
{
	transmit_enabled_ = enabled;

	qInfo() << "Transmit control" << (transmit_enabled_ ? "ENABLED" : "DISABLED");

	if (!transmit_enabled_)
		stopTransmit(QStringLiteral("transmit disabled"), true);
}

bool TciServer::transmitActive() const
{
	return tx_owner_ || tx_audio_owner_ || tx_watchdog_.isActive() || (radio_ && radio_->ptt());
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
		if (!transmit_enabled_) {
			qWarning().noquote()
				<< "PTT request denied because transmit is disabled:"
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

		tx_owner_ = socket;

		qInfo().noquote() << "PTT ON:" << reason;

		if (radio_)
			radio_->setPtt(true);

		armTxWatchdog(reason);

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

	stopTxAudioStream(QStringLiteral("PTT OFF"));
	disarmTxWatchdog();
	tx_owner_.clear();

	if (radio_)
		radio_->setPtt(false);

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

	const bool wasActive = tx_watchdog_.isActive();

	tx_watchdog_.start(max_tx_ms_);

	if (!wasActive) {
		qInfo().noquote()
			<< "TX watchdog armed:"
			<< max_tx_ms_
			<< "ms reason="
			<< reason;
	}
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
}

void TciServer::handleCommand(QWebSocket *socket, const TciCommand &command)
{
	const QString &name = command.name;
	const QStringList &a = command.args;

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
		}

		sendText(socket, QStringLiteral("modulation:0,%1;")
			.arg(radio_->mode().toLower()));
		return;
	}

	if (name == "trx" || name == "ptt")
	{
		if (a.size() < 2) {
			sendText(socket,
					 QStringLiteral("trx:0,%1;")
						 .arg(boolText(radio_->ptt())));
			return;
		}

		// The second argument is TX enabled.
		// The optional third argument selects the audio source.
		const QString value = a.at(1).trimmed().toLower();
		const QString source =
			a.size() >= 3 ? a.at(2).trimmed().toLower() : QString();

		const bool enabled =
			value == "true" || value == "1" ||
			value == "on" || value == "tx";

		if (enabled) {
			if (requestPtt(socket, true, command.raw)) {
				if (source == "tci") {
					if (tx_audio_owner_ != socket) {
						stopTxAudioStream(
							QStringLiteral("TX audio ownership change"));

						tx_audio_owner_ = socket;
						for (auto it = audio_clients_.begin(); it != audio_clients_.end(); ++it) {
							it.value().packetCount = 0;
							it.value().accumulator.clear();
						}
						tx_chrono_packet_count_ = 0;

						if (radio_)
							radio_->setPollingSuspended(true);

						emit txAudioStarted();

						// TX_CHRONO is the normal server-side audio clock.
						// Valid TX_AUDIO_STREAM frames are also accepted
						// without a preceding request for legacy interoperability.
						tx_chrono_samples_requested_ = 0;
						tx_chrono_clock_.start();
						tx_chrono_timer_.start();
						sendTxChrono();
					}
				} else {
					// TCI audio is not selected as the transmit source.
					stopTxAudioStream(
						QStringLiteral("TX source changed"));
				}
			}
		} else {
			requestPtt(socket, false, command.raw);
		}

		sendText(socket,
				 QStringLiteral("trx:0,%1;")
					 .arg(boolText(radio_->ptt())));
		return;
	}

	if (name == "split_enable") {
		if (a.size() >= 2) {
			const QString value = a.at(1).trimmed().toLower();

			const bool enabled =
				value == "true" ||
				value == "1" ||
				value == "on" ||
				value == "tx";

			radio_->setSplitEnabled(enabled);
			sendText(socket, QStringLiteral("split_enable:0,%1;")
								.arg(boolText(radio_->splitEnabled())));
			return;
		}

		sendText(socket, QStringLiteral("split_enable:0,%1;")
							.arg(boolText(radio_->splitEnabled())));
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
	    name == "audio_stream_samples")
	{
		// Audio settings belong to the requesting connection, even before
		// audio_start. A second client must never change another's stream.
		AudioClientState &state = audio_clients_[socket];

		if (name == "audio_samplerate") {
			// The current Qt audio backend delivers 48-kHz PCM. Do not
			// claim sample rates that are not actually resampled.
			if (a.size() == 1) {
				bool ok = false;
				const quint32 requested = a.at(0).toUInt(&ok);
				if (ok && TciSampleRate::isSupported(requested) &&
				    state.decimator.configure(requested)) {
					state.sampleRate = requested;
					state.accumulator.clear();
				}
			}
			sendText(socket, QStringLiteral("audio_samplerate:%1;").arg(state.sampleRate));
			return;
		}

		if (name == "audio_stream_sample_type") {
			if (a.size() == 1) {
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
			if (a.size() == 1) {
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
			if (a.size() == 1) {
				bool ok = false;
				const int requested = a.at(0).toInt(&ok);
				if (ok) {
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

	constexpr quint64 sampleRate = 48000;
	constexpr quint32 samplesPerFrame = 512;
	// A timer callback can be late on a loaded Pi. Drop overdue requests
	// instead of sending a burst that would flood the TX audio queue.
	const quint64 samplesElapsed =
		static_cast<quint64>(tx_chrono_clock_.nsecsElapsed()) * sampleRate / 1000000000ULL;
	if (tx_chrono_samples_requested_ > samplesElapsed)
		return;

	const QByteArray frame = TciStream::makeTxChronoFrame(
		0, static_cast<quint32>(sampleRate), TciStream::FLOAT32,
		1, samplesPerFrame, tx_chrono_packet_count_++);
	tx_audio_owner_->sendBinaryMessage(frame);
	tx_chrono_samples_requested_ = (samplesElapsed / samplesPerFrame + 1) * samplesPerFrame;
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

	if (forcePttOff || radio_->ptt()) {
		qWarning().noquote() << "Force unkey:" << reason;

		if (!radio_->online()) {
			qWarning() << "Radio is offline; will force PTT off when radio comes online";
			pending_unkey_on_radio_online_ = true;
			broadcastText(QStringLiteral("trx:0,false;"));
			return;
		}

		radio_->setPtt(false);
		broadcastText(QStringLiteral("trx:0,false;"));
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