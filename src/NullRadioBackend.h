#pragma once

#include "RadioBackend.h"

class NullRadioBackend final : public RadioBackend
{
	Q_OBJECT

public:
	explicit NullRadioBackend(QObject *parent = nullptr);

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

	bool online() const override { return true; }

private:
	qint64 frequency_hz_ = 14074000;
	qint64 tx_frequency_hz_ = 14074000;
	bool split_enabled_ = false;
	QString mode_ = "USB";
	bool ptt_ = false;
	bool polling_suspended_ = false;
};