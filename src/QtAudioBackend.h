#pragma once

#include "AudioBackend.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QAudioSink>
#include <QIODevice>
#include <QString>

#include <memory>

class QtAudioBackend final : public AudioBackend
{
	Q_OBJECT

public:
	explicit QtAudioBackend(QObject *parent = nullptr);

	void setRxDeviceName(const QString &name);
	void setTxDeviceName(const QString &name);
	void setDebug(bool enabled);

	bool startRx() override;
	void stopRx() override;

	bool startTx() override;
	void stopTx() override;
	void writeTxAudio(const QByteArray &pcm) override;

	int sampleRate() const;
	int channelCount() const;

	QAudioFormat rxFormat() const;
	QAudioFormat txFormat() const;

	static void listAudioDevices();

private slots:
	void onRxReadyRead();

private:
	bool chooseRxDevice(QAudioDevice *selected) const;
	bool chooseTxDevice(QAudioDevice *selected) const;

	QAudioFormat chooseAudioFormat(const QAudioDevice &device) const;
	static QString sampleFormatName(QAudioFormat::SampleFormat format);

	QString rx_device_name_;
	QString tx_device_name_;
	bool debug_ = false;

	QAudioDevice rx_device_;
	QAudioDevice tx_device_;

	QAudioFormat rx_format_;
	QAudioFormat tx_format_;

	std::unique_ptr<QAudioSource> rx_source_;
	std::unique_ptr<QAudioSink> tx_sink_;

	QIODevice *rx_io_ = nullptr;
	QIODevice *tx_io_ = nullptr;
};