#pragma once

#include "RadioBackend.h"

class NullRadioBackend final : public RadioBackend
{
	Q_OBJECT

public:
	explicit NullRadioBackend(QObject *parent = nullptr);

	qint64 frequencyHz() const override;
	void setFrequencyHz(qint64 hz) override;

	QString mode() const override;
	void setMode(const QString &mode) override;

	bool ptt() const override;
	void setPtt(bool enabled) override;

private:
	qint64 frequency_hz_ = 14074000;
	QString mode_ = "USB";
	bool ptt_ = false;
};