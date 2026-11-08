#pragma once

#include "RadioBackend.h"

#include <QThread>

class RigctldWorker;

class RigctldBackend final : public RadioBackend
{
	Q_OBJECT

public:
	explicit RigctldBackend(
		QString host,
		quint16 port,
		int pollMs = 250,
		QObject *parent = nullptr
	);

	~RigctldBackend() override;

	qint64 frequencyHz() const override;
	void setFrequencyHz(qint64 hz) override;

	qint64 txFrequencyHz() const override;
	void setTxFrequencyHz(qint64 hz) override;

	bool splitEnabled() const override;
	qint64 effectiveTxFrequencyHz() const override;
	void setSplitEnabled(bool enabled) override;

	QString mode() const override;
	void setMode(const QString &mode) override;

	bool ptt() const override;
	void setPtt(bool enabled) override;

	void setPollingSuspended(bool suspended) override;
	void setDebug(bool enabled);

	bool online() const override;
	void setRxMeterInterval(int milliseconds) override;
	void setTxMeterInterval(int milliseconds) override;
	void setFilterEdgesHzEnabled(bool enabled);
	QPair<int, int> rxFilterBand() const override;
	void requestRxFilterBand() override;
	void setRxFilterBand(int low, int high) override;
	QString agcMode() const override;
	void requestAgcReadback() override;
	void setAgcMode(const QString &mode) override;
	int drivePercent() const override;
	void requestDriveReadback() override;
	void setDrivePercent(int percent) override;
	std::optional<int> rxControlValue(RxControl setting) const override;
	void requestRxControl(RxControl setting) override;
	void setRxControl(RxControl setting, int value) override;
	QStringList supportedModes() const override;
	QPair<qint64, qint64> vfoLimits() const override;

	signals:
		void requestStart(QString host, quint16 port, int pollMs);
	void requestStop();

	void requestReadRxControl(RxControl setting);
	void requestSetRxControl(RxControl setting, int value);
	void requestRxMeterInterval(int milliseconds);
	void requestTxMeterInterval(int milliseconds);
	void requestFilterEdgesHzEnabled(bool enabled);
	void requestReadRxFilterBand();
	void requestSetRxFilterBand(int low, int high);
	void requestReadAgc();
	void requestSetAgcMode(const QString &mode);
	void requestReadDrive();
	void requestSetDrivePercent(int percent);
	void requestSetFrequencyHz(qint64 hz);
	void requestSetTxFrequencyHz(qint64 hz);
	void requestSetSplitEnabled(bool enabled);
	void requestSetMode(QString mode);
	void requestSetPtt(bool enabled);
	void requestSetPollingSuspended(bool suspended);
	void requestSetDebug(bool enabled);

private:
	void setupWorker();

	void updateFrequency(qint64 hz);
	void updateTxFrequency(qint64 hz);
	void updateSplit(bool enabled);
	void updateMode(const QString &mode);
	void updateSupportedModes(const QStringList &modes);
	void updateVfoLimits(qint64 minHz, qint64 maxHz);
	void updatePtt(bool enabled);

	void updateConnected(bool connected);
	void updateRadioUsable(bool usable);

private:
	QString host_;
	quint16 port_ = 4532;
	int poll_ms_ = 250;

	RigctldWorker *worker_ = nullptr;
	QThread worker_thread_;

	bool connected_ = false;
	bool radio_usable_ = false;

	qint64 frequency_hz_ = 0;
	qint64 tx_frequency_hz_ = 0;
	bool split_enabled_ = false;
	bool split_known_ = false;
	RxControlValues rx_controls_;
	int drive_percent_ = -1;
	QString agc_mode_;
	QPair<int, int> rx_filter_band_;
	QString mode_ = QStringLiteral("USB");
	QStringList supported_modes_;
	QPair<qint64, qint64> vfo_limits_{0, 0};
	bool ptt_ = false;

	bool polling_suspended_ = false;
	bool debug_ = false;
};