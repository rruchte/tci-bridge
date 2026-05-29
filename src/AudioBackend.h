#pragma once

#include <QObject>
#include <QByteArray>

class AudioBackend : public QObject
{
	Q_OBJECT

public:
	explicit AudioBackend(QObject *parent = nullptr)
		: QObject(parent)
	{
	}

	~AudioBackend() override = default;

	virtual bool startRx() = 0;
	virtual void stopRx() = 0;

	virtual bool startTx() = 0;
	virtual void stopTx() = 0;
	virtual void writeTxAudio(const QByteArray &pcm) = 0;

	signals:
		void rxAudioFrame(QByteArray pcm);
};