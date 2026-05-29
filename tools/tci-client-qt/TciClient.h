#pragma once

#include <QObject>
#include <QUrl>
#include <QWebSocket>
#include <QTimer>

class TciClient final : public QObject
{
	Q_OBJECT

public:
	explicit TciClient(QObject *parent = nullptr);

	void connectToServer(const QUrl &url);
	void disconnectFromServer();

	bool isConnected() const;

	void setFrequency(qint64 hz);
	void queryFrequency();

	void setMode(const QString &mode);
	void queryMode();

	void setPtt(bool enabled);
	void queryPtt();

	void configureAudio(
		int sampleRate = 48000,
		int channels = 1,
		int samplesPerFrame = 512
	);

	void startRxAudio();
	void stopRxAudio();

	void startTxAudio();
	void stopTxAudio();
	void sendTxAudioMonoInt16(const QByteArray &pcm);

	signals:
		void connected();
	void disconnected();
	void ready();
	void error(QString message);

	void frequencyChanged(qint64 hz);
	void modeChanged(QString mode);
	void pttChanged(bool enabled);

	void rxAudioFrame(QByteArray monoInt16Pcm, int sampleRate);

private slots:
	void onConnected();
	void onDisconnected();
	void onTextMessageReceived(const QString &message);
	void onBinaryMessageReceived(const QByteArray &message);
	void onErrorOccurred(QAbstractSocket::SocketError error);

private:
	void sendText(QString message);
	void parseTextMessage(const QString &message);

	static QString commandName(const QString &message);
	static QStringList commandArgs(const QString &message);
	static QString semicolon(QString message);

private:
	QWebSocket socket_;

	bool connected_ = false;
	bool ready_ = false;

	int audio_sample_rate_ = 48000;
	int audio_channels_ = 1;
	int audio_samples_per_frame_ = 512;
};