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
#include "TciSampleRate.h"

class TciServer final : public QObject
{
	Q_OBJECT

public:
	explicit TciServer(RadioBackend *radio, QObject *parent = nullptr);
	~TciServer() override;

	void setAllowPartialTxSensors(bool enabled) { allow_partial_tx_sensors_ = enabled; }
	bool listen(const QHostAddress &address, quint16 port);
	quint16 serverPort() const { return server_.serverPort(); }
	void sendRxAudio(const QByteArray &pcm, int sampleRate, int channels);
	void setTransmitEnabled(bool enabled);
	void setMaxTxMs(int maxTxMs);
	void setDefaultTxPrebufferMs(int ms) { default_tx_prebuffer_ms_ = ms; }
	void setUnkeyOnDisconnect(bool enabled);
	void forceUnkey(const QString &reason);
	void txAudioOutputReady();
	void setDebug(bool enabled);
	void setQuiet(bool enabled);
	void setTxTimingLogEnabled(bool enabled);
	bool transmitActive() const;

signals:
	void txAudioFrame(QByteArray pcm);
	void txAudioStarted(int prebufferMs);
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
	void broadcastTxFrequency();
	void updateRxMeterInterval();
	void updateTxMeterInterval();
	void sendTxMeterReading(const TxMeterReading &reading);
	void sendRxSignalLevel(int dbm);
	qint64 last_tx_frequency_ = 0;
	QString txEnableMessage() const;
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
	int default_tx_prebuffer_ms_ = 50;

	bool unkey_pending_ = false;

	bool debug_ = false;
	bool quiet_ = false;
	bool tx_timing_log_enabled_ = false;
	QSet<QString> warned_unknown_commands_;

	QWebSocketServer server_;
	QSet<QWebSocket *> clients_;
	struct RxMeterSubscription {
		int intervalMs;
		QElapsedTimer lastSent;
		std::optional<int> lastDbm;
	};
	QHash<QWebSocket *, RxMeterSubscription> rx_meter_clients_;
	int rx_meter_interval_ms_ = 0;
	struct TxMeterSubscription {
		int intervalMs;
		QElapsedTimer lastSent;
	};
	QHash<QWebSocket *, TxMeterSubscription> tx_meter_clients_;
	int tx_meter_interval_ms_ = 0;
	bool allow_partial_tx_sensors_ = false;
	QPointer<QWebSocket> tx_owner_;
	QPointer<QWebSocket> tx_audio_owner_;
	RadioBackend *radio_ = nullptr;
	struct AudioClientState
	{
		quint32 sampleRate = TciStream::DEFAULT_RX_SAMPLE_RATE;
		quint32 sampleType = TciStream::DEFAULT_RX_SAMPLE_TYPE;
		quint32 channels = TciStream::DEFAULT_RX_CHANNELS;
		quint32 samplesPerFrame = TciStream::DEFAULT_RX_SAMPLES_PER_FRAME;
		bool samplesExplicit = false;
		int txPrebufferMs = -1;
		quint32 packetCount = 0;
		QByteArray accumulator;
		TciSampleRate::RxDecimator decimator;
	};
	QHash<QWebSocket *, AudioClientState> audio_clients_;
	QSet<QWebSocket *> audio_stream_clients_;
	QTimer tx_watchdog_;
	QTimer tx_chrono_timer_;
	QElapsedTimer tx_chrono_clock_;
	quint64 tx_chrono_samples_requested_ = 0;

	quint32 tx_chrono_packet_count_ = 0;
	TciSampleRate::TxInterpolator tx_interpolator_;
	quint32 tx_input_sample_rate_ = 48000;
};