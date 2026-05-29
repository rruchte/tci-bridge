#pragma once

#include <QObject>
#include <QSet>
#include <QWebSocketServer>
#include <QWebSocket>

#include "RadioBackend.h"
#include "TciCommand.h"
#include "TciStreamFrame.h"

class TciServer final : public QObject
{
	Q_OBJECT

public:
	explicit TciServer(RadioBackend *radio, QObject *parent = nullptr);
	~TciServer() override;

	bool listen(const QHostAddress &address, quint16 port);
	void sendRxAudio(const QByteArray &pcm, int sampleRate, int channels);
	void setTxAudioKeysPtt(bool enabled);

signals:
	void txAudioFrame(QByteArray pcm);

private slots:
	void onNewConnection();
	void onTextMessageReceived(const QString &message);
	void onBinaryMessageReceived(const QByteArray &message);
	void onSocketDisconnected();

	void onFrequencyChanged(qint64 hz);
	void onModeChanged(const QString &mode);
	void onPttChanged(bool enabled);

private:
	void sendStartupBurst(QWebSocket *socket);
	void sendState(QWebSocket *socket);
	void handleCommand(QWebSocket *socket, const TciCommand &command);

	void sendText(QWebSocket *socket, const QString &message);
	void broadcastText(const QString &message);

	static QString boolText(bool value);
	static QString semicolon(QString message);

	bool tx_audio_keys_ptt_ = false;

	QWebSocketServer server_;
	QSet<QWebSocket *> clients_;
	RadioBackend *radio_ = nullptr;
	QSet<QWebSocket *> audio_clients_;

	quint32 audio_sample_rate_ = 48000;
	quint32 audio_sample_type_ = TciStream::INT16;
	quint32 audio_channels_ = 1;
	quint32 audio_samples_per_frame_ = 512;

	QByteArray audio_accumulator_;
};