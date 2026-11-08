#pragma once

#include "RxControl.h"
#include "TxMeterReading.h"

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>

class RigctldWorker final : public QObject
{
	Q_OBJECT

public:
	explicit RigctldWorker(QObject *parent = nullptr);
	~RigctldWorker() override;

public slots:
	void start(QString host, quint16 port, int pollMs);
	void stop();

	void readRxControl(RxControl setting);
	void setRxControl(RxControl setting, int value);
	void setTxMeterInterval(int milliseconds);
	void setRxMeterInterval(int milliseconds);
	void setFilterEdgesHzEnabled(bool enabled);
	void readRxFilterBand();
	void setRxFilterBand(int low, int high);
	void readAgc();
	void setAgcMode(const QString &mode);
	void readDrive();
	void setDrivePercent(int percent);
	void setFrequencyHz(qint64 hz);
	void setTxFrequencyHz(qint64 hz);
	void setSplitEnabled(bool enabled);
	void setMode(QString mode);
	void setPtt(bool enabled);

	void setPollingSuspended(bool suspended);
	void setDebug(bool enabled);

	void poll();

signals:
	void rxControlChanged(RxControl setting, int value, bool known);
	void rxSignalLevel(int dbm);
	void txMeterReading(const TxMeterReading &reading);
	void rxFilterBandChanged(int low, int high);
	void agcModeChanged(const QString &mode);
	void driveChanged(int percent);
	void frequencyChanged(qint64 hz);
	void txFrequencyChanged(qint64 hz);
	void splitChanged(bool enabled);
	void splitStateUnavailable();
	void modeChanged(QString mode);
	void supportedModesChanged(QStringList modes);
	void vfoLimitsChanged(qint64 minHz, qint64 maxHz);
	void pttChanged(bool enabled);
	void pttCommandFailed(bool requested);
	void pttCommandSucceeded(bool requested);

	void connectedChanged(bool connected);
	void error(QString message);
	void slowCall(QString operation, qint64 elapsedMs);

	void radioUsableChanged(bool usable);

private:
	bool ensureConnected();

	QString transact(const QString &command,
					 int timeoutMs,
					 const QString &operation);

	QStringList transactLines(const QString &command,
							  int timeoutMs,
							  const QString &operation);

	bool transactRprt(const QString &command,
					  int timeoutMs,
					  const QString &operation, bool *modeRejected = nullptr);

	bool transactOptional(const QString &command,
						  QString &response,
						  const QString &description, int timeoutMs = 75);

	bool pollFrequency();
	bool pollMode();
	bool pollPtt();

	void pollTxFrequencyBestEffort();
	void pollSplitBestEffort();
	void pollRxControl(RxControl setting, bool force = false);
	void pollDrive(bool force = false);
	QPair<int, int> readFilterSlopes(int timeoutMs);
	void updateRxFilterBand(QPair<int, int> slopes, bool force = false);
	void pollRxMeter();
	void pollTxMeter();
	std::optional<double> readTxMeter(const QString &level, bool &unsupported);
	bool pollAgc(bool force = false, QString *observed = nullptr);
	void querySupportedModes();
	void queryVfoLimits();

	void notePollSuccess();
	void notePollFailure(const QString &operation, const QString &response = QString());
	void setRadioUsable(bool usable, const QString &reason = QString());
	void forceReconnect(const QString &reason);

	static QString normalizeModeForRigctld(const QString &mode);
	static QString normalizeModeFromRigctld(const QString &mode);

private:
	QString host_;
	quint16 port_ = 4532;
	int poll_ms_ = 250;

	QTcpSocket *socket_ = nullptr;
	QTimer *poll_timer_ = nullptr;

	bool connected_ = false;
	bool radio_usable_ = false;
	int consecutive_poll_failures_ = 0;
	int max_consecutive_poll_failures_ = 3;
	
	bool debug_ = false;
	bool polling_suspended_ = false;

	QElapsedTimer split_poll_clock_;
	QElapsedTimer rx_control_poll_clock_;
	size_t next_rx_control_ = 0;
	int drive_percent_ = -1;
	bool drive_unsupported_ = false;
	QString agc_mode_;
	bool agc_unsupported_ = false;
	int rx_meter_interval_ms_ = 0;
	bool rx_meter_unsupported_ = false;
	QElapsedTimer rx_meter_clock_;
	int tx_meter_interval_ms_ = 0;
	bool tx_watts_unsupported_ = false;
	bool tx_swr_unsupported_ = false;
	QElapsedTimer tx_meter_clock_;
	bool filter_edges_hz_enabled_ = false;
	QPair<int, int> rx_filter_band_;
	RxControlValues rx_controls_;
	std::array<bool, rxControlCount> rx_control_unsupported_{};
	bool split_known_ = false;
	bool modes_queried_ = false;
	bool vfo_limits_queried_ = false;
	bool split_query_works_ = true;

	qint64 frequency_hz_ = 0;
	qint64 tx_frequency_hz_ = 0;
	bool split_enabled_ = false;
	QString mode_ = QStringLiteral("USB");
	bool ptt_ = false;
};