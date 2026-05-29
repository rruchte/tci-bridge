#include "NullRadioBackend.h"

NullRadioBackend::NullRadioBackend(QObject *parent)
	: RadioBackend(parent)
{
}

qint64 NullRadioBackend::frequencyHz() const
{
	return frequency_hz_;
}

void NullRadioBackend::setFrequencyHz(qint64 hz)
{
	if (hz <= 0 || frequency_hz_ == hz)
		return;

	frequency_hz_ = hz;
	emit frequencyChanged(frequency_hz_);
}

qint64 NullRadioBackend::txFrequencyHz() const
{
	return tx_frequency_hz_;
}

void NullRadioBackend::setTxFrequencyHz(qint64 hz)
{
	if (hz <= 0 || tx_frequency_hz_ == hz)
		return;

	tx_frequency_hz_ = hz;
	emit txFrequencyChanged(tx_frequency_hz_);
}

bool NullRadioBackend::splitEnabled() const
{
	return split_enabled_;
}

void NullRadioBackend::setSplitEnabled(bool enabled)
{
	if (split_enabled_ == enabled)
		return;

	split_enabled_ = enabled;
	emit splitChanged(split_enabled_);
}

QString NullRadioBackend::mode() const
{
	return mode_;
}

void NullRadioBackend::setMode(const QString &mode)
{
	const QString normalized = mode.trimmed().toUpper();

	if (normalized.isEmpty() || mode_ == normalized)
		return;

	mode_ = normalized;
	emit modeChanged(mode_);
}

bool NullRadioBackend::ptt() const
{
	return ptt_;
}

void NullRadioBackend::setPtt(bool enabled)
{
	if (ptt_ == enabled)
		return;

	ptt_ = enabled;
	emit pttChanged(ptt_);
}

void NullRadioBackend::setPollingSuspended(bool suspended)
{
	polling_suspended_ = suspended;
}
