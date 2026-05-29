#pragma once

#include <QAudioFormat>
#include <QByteArray>

namespace PcmConvert
{
	QByteArray toMonoInt16(
		const QByteArray &input,
		const QAudioFormat &format
	);

	QByteArray monoInt16ToFormat(
		const QByteArray &monoInt16,
		const QAudioFormat &targetFormat
	);

	bool isSupportedInputFormat(const QAudioFormat &format);
	bool isSupportedOutputFormat(const QAudioFormat &format);

	QString describeFormat(const QAudioFormat &format);
}