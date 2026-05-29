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
	void setSplitEnabled(bool enabled) override;

	QString mode() const override;
	void setMode(const QString &mode) override;

	bool ptt() const override;
	void setPtt(bool enabled) override;

	void setPollingSuspended(bool suspended) override;
	void setDebug(bool enabled);

	signals:
		void requestStart(QString host, quint16 port, int pollMs);
	void requestStop();

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
	void updatePtt(bool enabled);

private:
	QString host_;
	quint16 port_ = 4532;
	int poll_ms_ = 250;

	RigctldWorker *worker_ = nullptr;
	QThread worker_thread_;

	qint64 frequency_hz_ = 0;
	qint64 tx_frequency_hz_ = 0;
	bool split_enabled_ = false;
	QString mode_ = QStringLiteral("USB");
	bool ptt_ = false;

	bool polling_suspended_ = false;
	bool debug_ = false;
};