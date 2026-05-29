#include "TciServer.h"

#include <QDebug>

TciServer::TciServer(RadioBackend *radio, QObject *parent) :
	QObject(parent), server_(QStringLiteral("tci-bridge"), QWebSocketServer::NonSecureMode, this), radio_(radio)
{
	connect(&server_, &QWebSocketServer::newConnection, this, &TciServer::onNewConnection);

	connect(radio_, &RadioBackend::frequencyChanged, this, &TciServer::onFrequencyChanged);

	connect(radio_, &RadioBackend::modeChanged, this, &TciServer::onModeChanged);

	connect(radio_, &RadioBackend::pttChanged, this, &TciServer::onPttChanged);
}

TciServer::~TciServer()
{
	for (QWebSocket *client: std::as_const(clients_))
	{
		client->close();
		client->deleteLater();
	}

	clients_.clear();
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
	const TciStream::StreamFrame frame = TciStream::parseFrame(message);

	if (!frame.valid)
	{
		qWarning() << "Invalid TCI binary frame:" << message.size() << "bytes";
		return;
	}

	qInfo() << "RX binary stream frame:"
			<< "type=" << frame.streamType << "receiver=" << frame.receiver << "sampleRate=" << frame.sampleRate
			<< "sampleType=" << frame.sampleType << "channels=" << frame.channels << "samples=" << frame.sampleCount
			<< "payload=" << frame.payload.size();

	if (frame.streamType != TciStream::TX_AUDIO_STREAM)
	{
		qInfo() << "Ignoring non-TX audio stream frame:" << frame.streamType;
		return;
	}

	if (frame.sampleRate != 48000)
	{
		qWarning() << "Only 48000 Hz TX audio is currently supported; got sampleRate=" << frame.sampleRate;
		return;
	}

	if (frame.sampleType != TciStream::INT16)
	{
		qWarning() << "Only TX int16 audio is currently supported; got sampleType=" << frame.sampleType;
		return;
	}

	if (frame.channels != 1)
	{
		qWarning() << "Only mono TX audio is currently supported; got channels=" << frame.channels;
		return;
	}

	if (tx_audio_keys_ptt_ && !radio_->ptt())
		radio_->setPtt(true);

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

	socket->deleteLater();
}

void TciServer::onFrequencyChanged(qint64 hz)
{
	broadcastText(QStringLiteral("vfo:0,0,%1;").arg(hz));
	broadcastText(QStringLiteral("dds:0,0,%1;").arg(hz));
}

void TciServer::onModeChanged(const QString &mode)
{
	broadcastText(QStringLiteral("modulation:0,0,%1;").arg(mode.toLower()));
}

void TciServer::onPttChanged(bool enabled) { broadcastText(QStringLiteral("trx:0,%1;").arg(boolText(enabled))); }

void TciServer::sendStartupBurst(QWebSocket *socket)
{
	// This is deliberately conservative. The exact startup vocabulary varies
	// across clients, so phase 1 logs unknown commands and answers the basics.

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

void TciServer::sendState(QWebSocket *socket)
{
	sendText(socket, QStringLiteral("vfo:0,0,%1;").arg(radio_->frequencyHz()));
	sendText(socket, QStringLiteral("dds:0,0,%1;").arg(radio_->frequencyHz()));
	sendText(socket, QStringLiteral("modulation:0,0,%1;").arg(radio_->mode().toLower()));
	sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
}

void TciServer::handleCommand(QWebSocket *socket, const TciCommand &command)
{
	const QString &name = command.name;
	const QStringList &a = command.args;

	// Connection / discovery-ish commands.
	if (name == "start" || name == "ready" || name == "initialize" || name == "protocol" || name == "version")
	{
		sendText(socket, command.raw);
		sendState(socket);
		return;
	}

	// Common query forms: "vfo;" or "vfo:0,0;"
	if (name == "vfo" || name == "dds")
	{
		if (a.size() >= 3)
		{
			bool ok = false;
			const qint64 hz = a.at(2).toLongLong(&ok);

			if (ok)
			{
				radio_->setFrequencyHz(hz);
				sendText(socket, command.raw);
				return;
			}
		}

		sendText(socket, QStringLiteral("vfo:0,0,%1;").arg(radio_->frequencyHz()));
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

			radio_->setPtt(enabled);
			sendText(socket, command.raw);
			return;
		}

		sendText(socket, QStringLiteral("trx:0,%1;").arg(boolText(radio_->ptt())));
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

			/*
			if (type == "int16")
				audio_sample_type_ = TciStream::INT16;
			else if (type == "int24")
				audio_sample_type_ = TciStream::INT24;
			else if (type == "int32")
				audio_sample_type_ = TciStream::INT32;
			else if (type == "float32")
				audio_sample_type_ = TciStream::FLOAT32;
			else
				qWarning() << "Unsupported requested audio sample type:" << type;
			*/
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
		sendText(socket, command.raw);
		qInfo() << "TX audio start requested";
		if (tx_audio_keys_ptt_)
			radio_->setPtt(true);
		return;
	}

	if (name == "tx_audio_stop")
	{
		sendText(socket, command.raw);
		qInfo() << "TX audio stop requested";
		if (tx_audio_keys_ptt_)
			radio_->setPtt(false);
		return;
	}

	qWarning().noquote() << "Unhandled TCI command:" << command.raw << "name=" << command.name
						 << "args=" << command.args;

	// Echo anyway. Some TCI clients serialize command flow by waiting for
	// confirmation. Echoing unknown commands keeps discovery sessions alive.
	sendText(socket, command.raw);
}

void TciServer::sendText(QWebSocket *socket, const QString &message)
{
	if (!socket)
		return;

	const QString framed = semicolon(message);

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

		qInfo() << "BCAST RX audio frame:" << frame.size() << "bytes pcm=" << chunk.size()
				<< "sample_rate=" << audio_sample_rate_ << "channels=" << audio_channels_
				<< "samples=" << audio_samples_per_frame_;
	}
}

void TciServer::setTxAudioKeysPtt(bool enabled) { tx_audio_keys_ptt_ = enabled; }

void TciServer::broadcastText(const QString &message)
{
	const QString framed = semicolon(message);

	qInfo().noquote() << "BCAST text:" << framed;

	for (QWebSocket *client: std::as_const(clients_))
		client->sendTextMessage(framed);
}

QString TciServer::boolText(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }

QString TciServer::semicolon(QString message)
{
	message = message.trimmed();

	if (!message.endsWith(';'))
		message.append(';');

	return message;
}
