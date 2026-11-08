#pragma once

#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QElapsedTimer>

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
	void setTransmitEnabled(bool enabled);
	void setMaxTxMs(int maxTxMs);
	void setUnkeyOnDisconnect(bool enabled);
	void forceUnkey(const QString &reason);
	void setDebug(bool enabled);
	void setQuiet(bool enabled);
	void setTxTimingLogEnabled(bool enabled);
	void setTxChronoEnabled(bool enabled);
	bool transmitActive() const;

signals:
	void txAudioFrame(QByteArray pcm);
	void txAudioStarted();
	void txAudioStopped();

private slots:
	void onNewConnection();
	void onTextMessageReceived(const QString &message);
	void onBinaryMessageReceived(const QByteArray &message);
	void onSocketDisconnected();

	void onFrequencyChanged(qint64 hz);
	void onTxFrequencyChanged(qint64 hz);
	void onSplitChanged(bool enabled);
	void onModeChanged(const QString &mode);
	void onPttChanged(bool enabled);

	void onTxWatchdogExpired();
	void sendTxChrono();

	void onRadioOnlineChanged(bool online);

private:
	void sendStartupBurst(QWebSocket *socket);
	void sendState(QWebSocket *socket);
	void handleCommand(QWebSocket *socket, const TciCommand &command);

	void stopTxAudioStream(const QString &reason);
	void stopTxAudioStreamForSocket(QWebSocket *socket, const QString &reason);
	void stopTransmit(const QString &reason, bool forcePttOff);

	void sendText(QWebSocket *socket, const QString &message);
	void broadcastText(const QString &message);

	bool requestPtt(QWebSocket *socket, bool enabled, const QString &reason);
	bool clientCanTransmit(QWebSocket *socket, const QString &reason) const;
	void armTxWatchdog(const QString &reason);
	void disarmTxWatchdog();

	void logTxFrameTiming(const TciStream::StreamFrame &frame);


	static QString boolText(bool value);
	static QString semicolon(QString message);

	bool transmit_enabled_ = false;
	bool unkey_on_disconnect_ = true;
	int max_tx_ms_ = 30000;

	bool pending_unkey_on_radio_online_ = false;

	bool debug_ = false;
	bool quiet_ = false;
	bool tx_timing_log_enabled_ = false;
	QSet<QString> warned_unknown_commands_;

	QWebSocketServer server_;
	QSet<QWebSocket *> clients_;
	QPointer<QWebSocket> tx_owner_;
	QPointer<QWebSocket> tx_audio_owner_;
	RadioBackend *radio_ = nullptr;
	struct AudioClientState
	{
		quint32 sampleRate = 48000;
		quint32 sampleType = TciStream::INT16;
		quint32 channels = 1;
		quint32 samplesPerFrame = 512;
		quint32 packetCount = 0;
		QByteArray accumulator;
	};
	QHash<QWebSocket *, AudioClientState> audio_clients_;
	QSet<QWebSocket *> audio_stream_clients_;
	QTimer tx_watchdog_;
	QTimer tx_chrono_timer_;
	QElapsedTimer tx_chrono_clock_;
	quint64 tx_chrono_samples_requested_ = 0;
	bool tx_chrono_enabled_ = false;

	quint32 tx_chrono_packet_count_ = 0;
};