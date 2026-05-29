#include "TciClient.h"
#include "TciStreamFrame.h"

#include <QDebug>

TciClient::TciClient(QObject *parent)
    : QObject(parent)
{
    connect(&socket_, &QWebSocket::connected,
            this, &TciClient::onConnected);

    connect(&socket_, &QWebSocket::disconnected,
            this, &TciClient::onDisconnected);

    connect(&socket_, &QWebSocket::textMessageReceived,
            this, &TciClient::onTextMessageReceived);

    connect(&socket_, &QWebSocket::binaryMessageReceived,
            this, &TciClient::onBinaryMessageReceived);

    connect(&socket_,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this,
            &TciClient::onErrorOccurred);
}

void TciClient::connectToServer(const QUrl &url)
{
    qInfo() << "Connecting to TCI server:" << url;
    socket_.open(url);
}

void TciClient::disconnectFromServer()
{
    socket_.close();
}

bool TciClient::isConnected() const
{
    return connected_;
}

void TciClient::setFrequency(qint64 hz)
{
    sendText(QStringLiteral("vfo:0,0,%1;").arg(hz));
}

void TciClient::queryFrequency()
{
    sendText("vfo;");
}

void TciClient::setMode(const QString &mode)
{
    sendText(QStringLiteral("modulation:0,0,%1;").arg(mode.trimmed().toLower()));
}

void TciClient::queryMode()
{
    sendText("modulation;");
}

void TciClient::setPtt(bool enabled)
{
    sendText(QStringLiteral("trx:0,%1;").arg(enabled ? "true" : "false"));
}

void TciClient::queryPtt()
{
    sendText("trx;");
}

void TciClient::configureAudio(int sampleRate, int channels, int samplesPerFrame)
{
    audio_sample_rate_ = sampleRate;
    audio_channels_ = channels;
    audio_samples_per_frame_ = samplesPerFrame;

    sendText("audio_stream_sample_type:int16;");
    sendText(QStringLiteral("audio_stream_channels:%1;").arg(audio_channels_));
    sendText(QStringLiteral("audio_stream_samples:%1;").arg(audio_samples_per_frame_));
    sendText(QStringLiteral("audio_samplerate:%1;").arg(audio_sample_rate_));
}

void TciClient::startRxAudio()
{
    sendText("audio_start:0;");
}

void TciClient::stopRxAudio()
{
    sendText("audio_stop:0;");
}

void TciClient::startTxAudio()
{
    sendText("tx_audio_start:0;");
}

void TciClient::stopTxAudio()
{
    sendText("tx_audio_stop:0;");
}

void TciClient::sendTxAudioMonoInt16(const QByteArray &pcm)
{
    if (!connected_)
        return;

    const QByteArray frame = TciStream::makeTxAudioFrame(
        pcm,
        0,
        static_cast<quint32>(audio_sample_rate_),
        TciStream::INT16,
        static_cast<quint32>(audio_channels_)
    );

    socket_.sendBinaryMessage(frame);
}

void TciClient::onConnected()
{
    connected_ = true;
    qInfo() << "Connected to TCI server";
    emit connected();
}

void TciClient::onDisconnected()
{
    connected_ = false;
    ready_ = false;
    qInfo() << "Disconnected from TCI server";
    emit disconnected();
}

void TciClient::onTextMessageReceived(const QString &message)
{
    qInfo().noquote() << "TCI RX text:" << message;
    parseTextMessage(message);
}

void TciClient::onBinaryMessageReceived(const QByteArray &message)
{
    const TciStream::StreamFrame frame = TciStream::parseFrame(message);

    if (!frame.valid) {
        qWarning() << "Invalid TCI binary frame:" << message.size();
        return;
    }

    if (frame.streamType != TciStream::RX_AUDIO_STREAM) {
        qInfo() << "Ignoring non-RX stream type:" << frame.streamType;
        return;
    }

    if (frame.sampleType != TciStream::INT16 ||
        frame.channels != 1 ||
        frame.sampleRate != 48000) {
        qWarning() << "Unsupported RX audio frame:"
                   << "sampleRate=" << frame.sampleRate
                   << "sampleType=" << frame.sampleType
                   << "channels=" << frame.channels;
        return;
    }

    const qsizetype expectedBytes =
        static_cast<qsizetype>(frame.sampleCount) * 2;

    if (frame.payload.size() < expectedBytes) {
        qWarning() << "Short RX audio payload:"
                   << frame.payload.size()
                   << "expected"
                   << expectedBytes;
        return;
    }

    const QByteArray pcm = frame.payload.left(expectedBytes);

    emit rxAudioFrame(pcm, static_cast<int>(frame.sampleRate));
}

void TciClient::onErrorOccurred(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);

    const QString msg = socket_.errorString();
    qWarning() << "TCI socket error:" << msg;
    emit this->error(msg);
}

void TciClient::sendText(QString message)
{
    if (!connected_) {
        qWarning() << "Cannot send TCI text while disconnected:" << message;
        return;
    }

    message = semicolon(message);

    qInfo().noquote() << "TCI TX text:" << message;
    socket_.sendTextMessage(message);
}

void TciClient::parseTextMessage(const QString &message)
{
    const QString name = commandName(message);
    const QStringList args = commandArgs(message);

    if (name == "ready") {
        if (!ready_) {
            ready_ = true;
            emit ready();
        }
        return;
    }

    if (name == "vfo" || name == "dds") {
        if (args.size() >= 3) {
            bool ok = false;
            const qint64 hz = args.at(2).toLongLong(&ok);

            if (ok)
                emit frequencyChanged(hz);
        }

        return;
    }

    if (name == "modulation" || name == "mode") {
        if (args.size() >= 3)
            emit modeChanged(args.at(2).trimmed().toUpper());

        return;
    }

    if (name == "trx" || name == "ptt") {
        if (args.size() >= 2) {
            const QString value = args.last().trimmed().toLower();
            emit pttChanged(
                value == "true" ||
                value == "1" ||
                value == "on" ||
                value == "tx"
            );
        }

        return;
    }
}

QString TciClient::commandName(const QString &message)
{
    QString s = message.trimmed();

    if (s.endsWith(';'))
        s.chop(1);

    const int colon = s.indexOf(':');

    if (colon < 0)
        return s.trimmed().toLower();

    return s.left(colon).trimmed().toLower();
}

QStringList TciClient::commandArgs(const QString &message)
{
    QString s = message.trimmed();

    if (s.endsWith(';'))
        s.chop(1);

    const int colon = s.indexOf(':');

    if (colon < 0)
        return {};

    const QString argString = s.mid(colon + 1).trimmed();

    if (argString.isEmpty())
        return {};

    QStringList out;

    const auto parts = argString.split(',', Qt::KeepEmptyParts);

    for (const QString &part : parts)
        out << part.trimmed();

    return out;
}

QString TciClient::semicolon(QString message)
{
    message = message.trimmed();

    if (!message.endsWith(';'))
        message.append(';');

    return message;
}