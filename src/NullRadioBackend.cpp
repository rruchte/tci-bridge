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