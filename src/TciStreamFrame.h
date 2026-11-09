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
		quint32 count = 0; // TCI 2.1 packet counter, offset 32

		QByteArray payload;
	};

	QByteArray makeRxAudioFrame(
		const QByteArray &pcm,
		quint32 receiver,
		quint32 sampleRate,
		quint32 sampleType,
		quint32 channels,
		quint32 count = 0
	);

	QByteArray makeTxChronoFrame(quint32 receiver, quint32 sampleRate,
	                             quint32 sampleType, quint32 channels,
	                             quint32 samples, quint32 count);

	// Converts native 48-kHz mono signed int16 PCM into the negotiated
	// uncompressed audio sample format and channel count.
	QByteArray convertRxMonoInt16(const QByteArray &monoPcm,
	                               quint32 sampleType, quint32 channels);

	// Decode uncompressed TCI TX PCM to 48-kHz mono signed int16.
	// Stereo pairs are mixed to mono. Returns empty on invalid input.
	QByteArray convertTxToMonoInt16(const StreamFrame &frame);

	StreamFrame parseFrame(const QByteArray &frame);

	int bytesPerSample(quint32 sampleType);
}