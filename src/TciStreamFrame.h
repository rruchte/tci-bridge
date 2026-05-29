#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace TciStream
{
	enum StreamType : quint32
	{
		IQ_STREAM = 0,
		RX_AUDIO_STREAM = 1,
		TX_AUDIO_STREAM = 2,
		TX_CHRONO = 3,
		LINEOUT_STREAM = 4
	};

	enum SampleType : quint32
	{
		INT16 = 0,
		INT24 = 1,
		INT32 = 2,
		FLOAT32 = 3
	};

	struct StreamFrame
	{
		bool valid = false;

		quint32 receiver = 0;
		quint32 sampleRate = 0;
		quint32 sampleType = INT16;
		quint32 codec = 0;
		quint32 crc = 0;
		quint32 sampleCount = 0;
		quint32 streamType = RX_AUDIO_STREAM;
		quint32 channels = 1;

		QByteArray payload;
	};

	QByteArray makeRxAudioFrame(
		const QByteArray &pcm,
		quint32 receiver,
		quint32 sampleRate,
		quint32 sampleType,
		quint32 channels
	);

	StreamFrame parseFrame(const QByteArray &frame);

	int bytesPerSample(quint32 sampleType);
}