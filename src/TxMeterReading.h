#pragma once

#include <QMetaType>

#include <optional>

struct TxMeterReading
{
	std::optional<double> microphoneDbm;
	std::optional<double> outputWatts;
	std::optional<double> peakWatts;
	std::optional<double> swr;
};

Q_DECLARE_METATYPE(TxMeterReading)
