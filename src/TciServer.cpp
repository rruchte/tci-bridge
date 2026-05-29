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

	tx_watchdog_.setSingleShot(true);

	connect(&tx_watchdog_, &QTimer::timeout, this, &TciServer::onTxWatchdogExpired);
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

	if (frame.sampleType != TciStream::INT16) {
		qWarning() << "Only TX int16 audio is currently supported; got sampleType="
				   << frame.sampleType;
		return;
	}

	if (frame.channels != 1) {
		qWarning() << "Only mono TX audio is currently supported; got channels="
				   << frame.channels;
		return;
	}

	if (frame.payload.isEmpty()) {
		qWarning() << "Ignoring empty TX audio payload";
		return;
	}

	if (!clientCanTransmit(socket, "TX audio frame"))
		return;

	if (!tx_audio_owner_) {
		qWarning() << "TX audio denied because no client requested tx_audio_start";
		return;
	}

	if (tx_audio_owner_ != socket) {
		qWarning() << "TX audio denied because another client owns TX audio";
		return;
	}

	if (tx_audio_keys_ptt_ && !radio_->ptt() && !tx_owner_) {
		if (!requestPtt(socket, true, "TX audio frame"))
			return;
	}

	// Logical PTT state is enough. Rigctld hardware state may lag, and should
	// never block the audio path.
	if (radio_->ptt() || tx_owner_)
		armTxWatchdog("TX audio frame");

	logTxFrameTiming(frame);

	emit txAudioFrame(frame.payload);
}

void TciServer::onSocketDisconnected()
{
	auto *socket = qobject_cast<QWebSocket *>(sender());

	if (!socket)
		return;

	clients_.remove(socket);
	audio_clients_.remove(socket);

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
	broadcastText(QStringLiteral("dds:0,0,%1;").arg(hz));
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
	broadcastText(QStringLiteral("modulation:0,0,%1;").arg(mode.toLower()));
}

void TciServer::onPttChanged(bool enabled)
{
	broadcastText(QStringLiteral("trx:0,%1;").arg(boolText(enabled)));
}

void TciServer::sendStartupBurst(QWebSocket *socket)
{
	sendText(socket, "protocol:ExpertSDR3 TCI;");
	sendText(socket, "version:1.9;");
	sendText(socket, "device:0,tci-bridge;");
	sendText(socket, "receive_only:false;");
	sendText(socket, "trx_count:1;");
	sendText(socket, "channels_count:1;");
	sendText(socket, "vfo_limits:0,0,100000,60000000;");
	sendText(socket, "if_limits:0,0,-48000,48000;");
	sendText(socket, "ready;");
}

void TciServer::setTransmitEnabled(bool enabled)
{
	transmit_enabled_ = enabled;

	qInfo() << "Transmit control" << (transmit_enabled_ ? "ENABLED" : "DISABLED");

	if (!transmit_enabled_)
		stopTransmit(QStringLiteral("transmit disabled"), true);
}

void TciServer::setTxAudioKeysPtt(bool enabled)
{
	tx_audio_keys_ptt_ = enabled;

	qInfo() << "TX audio keys PTT" << (tx_audio_keys_ptt_ ? "ENABLED" : "DISABLED");
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

bool TciServer::requestPtt(QWebSocket *socket, bool enabled, const QString &reason)
{
	if (enabled && !transmit_enabled_) {
		qWarning().noquote()
			<< "PTT request denied because transmit is disabled:"
			<< reason;

		broadcastText("trx:0,false;");
		return false;
	}

	if (enabled) {
		if (tx_owner_ && tx_owner_ != socket) {
			qWarning().noquote()
				<< "PTT request denied because another client owns TX:"
				<< reason;

			if (socket)
				sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));

			return false;
		}

		tx_owner_ = socket;

		qWarning().noquote() << "PTT ON:" << reason;

		radio_->setPtt(true);
		armTxWatchdog(reason);
		return true;
	}

	qInfo().noquote() << "PTT OFF:" << reason;

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
	sendText(socket, QStringLiteral("dds:0,0,%1;").arg(radio_->frequencyHz()));
	sendText(socket, QStringLiteral("modulation:0,0,%1;").arg(radio_->mode().toLower()));
	sendText(socket, QStringLiteral("split_enable:0,%1;").arg(boolText(radio_->splitEnabled())));
	sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
}

void TciServer::handleCommand(QWebSocket *socket, const TciCommand &command)
{
	const QString &name = command.name;
	const QStringList &a = command.args;

	if (name == "start" || name == "ready" || name == "initialize" || name == "protocol" || name == "version")
	{
		sendText(socket, command.raw);
		sendState(socket);
		return;
	}

	if (name == "vfo" || name == "dds") {
		int channel = 0;

		if (a.size() >= 2) {
			bool channelOk = false;
			const int parsedChannel = a.at(1).toInt(&channelOk);

			if (channelOk)
				channel = parsedChannel;
		}

		if (a.size() >= 3) {
			bool ok = false;
			const qint64 hz = a.at(2).toLongLong(&ok);

			if (ok) {
				if (channel == 1)
					radio_->setTxFrequencyHz(hz);
				else
					radio_->setFrequencyHz(hz);

				sendText(socket, command.raw);
				return;
			}
		}

		if (channel == 1) {
			sendText(socket, QStringLiteral("vfo:0,1,%1;").arg(radio_->txFrequencyHz()));
		} else {
			sendText(socket, QStringLiteral("vfo:0,0,%1;").arg(radio_->frequencyHz()));
		}

		return;
	}

	if (name == "modulation" || name == "mode")
	{
		if (a.size() >= 3)
		{
			radio_->setMode(a.at(2));
			sendText(socket, command.raw);
			return;
		}

		sendText(socket, QStringLiteral("modulation:0,0,%1;").arg(radio_->mode().toLower()));
		return;
	}

	if (name == "trx" || name == "ptt")
	{
		if (a.size() >= 2)
		{
			const QString value = a.last().trimmed().toLower();

			const bool enabled = value == "true" || value == "1" || value == "on" || value == "tx";

			requestPtt(socket, enabled, QStringLiteral("client command: %1").arg(command.raw));
			sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
			return;
		}

		sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
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

	if (name == "stop" || name == "close")
	{
		sendText(socket, command.raw);
		socket->close();
		return;
	}

	if (name == "audio_start")
	{
		audio_clients_.insert(socket);
		sendText(socket, command.raw);
		qInfo() << "Enabled RX audio stream for client:" << socket->peerAddress().toString() << socket->peerPort();
		return;
	}

	if (name == "audio_stop")
	{
		audio_clients_.remove(socket);
		sendText(socket, command.raw);
		qInfo() << "Disabled RX audio stream for client:" << socket->peerAddress().toString() << socket->peerPort();
		return;
	}

	if (name == "audio_samplerate")
	{
		if (!a.isEmpty())
		{
			bool ok = false;
			const quint32 rate = a.at(0).toUInt(&ok);

			if (ok && rate > 0)
				audio_sample_rate_ = rate;
		}

		sendText(socket, QStringLiteral("audio_samplerate:%1;").arg(audio_sample_rate_));
		return;
	}

	if (name == "audio_stream_sample_type")
	{
		if (!a.isEmpty())
		{
			const QString type = a.at(0).trimmed().toLower();

			if (type == "int16")
			{
				audio_sample_type_ = TciStream::INT16;
			}
			else
			{
				qWarning() << "Only int16 is currently supported";
			}
		}

		QString typeName = "int16";

		if (audio_sample_type_ == TciStream::INT24)
			typeName = "int24";
		else if (audio_sample_type_ == TciStream::INT32)
			typeName = "int32";
		else if (audio_sample_type_ == TciStream::FLOAT32)
			typeName = "float32";

		sendText(socket, QStringLiteral("audio_stream_sample_type:%1;").arg(typeName));
		return;
	}

	if (name == "audio_stream_channels")
	{
		if (!a.isEmpty())
		{
			bool ok = false;
			const quint32 requested = a.at(0).toUInt(&ok);

			if (ok && (requested == 1 || requested == 2))
				audio_channels_ = requested;
			else
				qWarning() << "Unsupported requested audio channel count:" << a.at(0);
		}

		sendText(socket, QStringLiteral("audio_stream_channels:%1;").arg(audio_channels_));
		return;
	}

	if (name == "audio_stream_samples")
	{
		if (!a.isEmpty())
		{
			bool ok = false;
			const quint32 requested = a.at(0).toUInt(&ok);

			if (ok && requested >= 100 && requested <= 2048)
				audio_samples_per_frame_ = requested;
			else
				qWarning() << "Unsupported requested audio samples-per-frame:" << a.at(0);
		}

		sendText(socket, QStringLiteral("audio_stream_samples:%1;").arg(audio_samples_per_frame_));
		return;
	}

	if (name == "tx_audio_start")
	{
		if (tx_audio_owner_ && tx_audio_owner_ != socket) {
			qWarning() << "TX audio start denied because another client owns TX audio";
			sendText(socket, QStringLiteral("tx_audio_start:0,false;"));
			return;
		}

		tx_audio_owner_ = socket;

		if (radio_)
			radio_->setPollingSuspended(true);

		qInfo() << "TX audio start requested; owner =" << socket;

		emit txAudioStarted();

		sendText(socket, command.raw);

		if (tx_audio_keys_ptt_)
			requestPtt(socket, true, "tx_audio_start");

		return;
	}

	if (name == "tx_audio_stop")
	{
		stopTxAudioStreamForSocket(socket, QStringLiteral("tx_audio_stop"));

		sendText(socket, command.raw);

		if (tx_audio_keys_ptt_)
			requestPtt(socket, false, "tx_audio_stop");

		return;
	}

	if (isCompatibilityEchoCommand(name)) {
		if (debug_) {
			qInfo().noquote()
				<< "Compatibility echo for unsupported TCI command:"
				<< command.raw;
		}

		sendText(socket, command.raw);
		return;
	}

	if (!warned_unknown_commands_.contains(name)) {
		warned_unknown_commands_.insert(name);

		qWarning().noquote()
			<< "Unhandled TCI command, echoing for compatibility:"
			<< command.raw
			<< "name=" << command.name
			<< "args=" << command.args;
	} else if (debug_) {
		qInfo().noquote()
			<< "Repeated unhandled TCI command, echoing:"
			<< command.raw;
	}

	sendText(socket, command.raw);
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
	if (audio_clients_.isEmpty())
		return;

	if (pcm.isEmpty())
		return;

	audio_sample_rate_ = static_cast<quint32>(sampleRate);
	audio_channels_ = static_cast<quint32>(channels);

	audio_accumulator_.append(pcm);

	const int bps = TciStream::bytesPerSample(audio_sample_type_);
	const int bytesPerFrame = static_cast<int>(audio_samples_per_frame_) * bps * static_cast<int>(audio_channels_);

	if (bytesPerFrame <= 0)
		return;

	while (audio_accumulator_.size() >= bytesPerFrame)
	{
		const QByteArray chunk = audio_accumulator_.left(bytesPerFrame);
		audio_accumulator_.remove(0, bytesPerFrame);

		const QByteArray frame =
				TciStream::makeRxAudioFrame(chunk, 0, audio_sample_rate_, audio_sample_type_, audio_channels_);

		for (QWebSocket *client: std::as_const(audio_clients_))
		{
			client->sendBinaryMessage(frame);
		}
	}
}

void TciServer::stopTxAudioStream(const QString &reason)
{
	if (!tx_audio_owner_)
		return;

	qInfo() << "TX audio stream stopped:"
			<< reason;

	tx_audio_owner_ = nullptr;

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

	if (radio_)
		radio_->setPollingSuspended(false);

	if (!radio_)
		return;

	if (forcePttOff || radio_->ptt()) {
		qWarning().noquote() << "Force unkey:" << reason;
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

bool TciServer::isCompatibilityEchoCommand(const QString &name) const
{
	static const QSet<QString> commands = {
		QStringLiteral("rx_enable"),
		QStringLiteral("rx_sensitivity"),
		QStringLiteral("rx_filter_band"),
		QStringLiteral("rx_filter_enable"),

		QStringLiteral("agc_mode"),
		QStringLiteral("agc_gain"),
		QStringLiteral("preamp"),
		QStringLiteral("attenuator"),

		QStringLiteral("volume"),
		QStringLiteral("mute"),
		QStringLiteral("drive"),
		QStringLiteral("mon"),

		QStringLiteral("rit_enable"),
		QStringLiteral("xit_enable"),
		QStringLiteral("rit_offset"),
		QStringLiteral("xit_offset"),
		QStringLiteral("if"),
		QStringLiteral("if_shift"),

		QStringLiteral("spot"),
		QStringLiteral("tune"),

		QStringLiteral("waterfall_start"),
		QStringLiteral("waterfall_stop"),
		QStringLiteral("iq_start"),
		QStringLiteral("iq_stop")
	};

	return commands.contains(name);
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