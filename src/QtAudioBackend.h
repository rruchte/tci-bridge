#pragma once

#include "AudioBackend.h"
#include "AudioDeviceResolver.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <QTimer>

#include <memory>

enum class AudioSelectionMode {
	Default,
	Manual,
	AutoUsbFullDuplex
};

class QtAudioBackend final : public AudioBackend
{
	Q_OBJECT

public:
	explicit QtAudioBackend(QObject *parent = nullptr);
	~QtAudioBackend() override;

	void setRxDeviceName(const QString &name);
	void setTxDeviceName(const QString &name);
	void setDebug(bool enabled);
	void setQuiet(bool enabled);

	void setTxSinkBufferMs(int ms);
	void setTxPrebufferMs(int ms);
	void setTxJitterBufferMs(int ms);
	void setTxDrainIntervalMs(int ms);

	void setAudioSelectionMode(const QString &mode);

	bool startRx() override;
	void stopRx() override;

	bool startTx() override;
	void stopTx() override;
	void writeTxAudio(const QByteArray &pcm) override;

	void beginTxAudioStream();
	void endTxAudioStream();

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
	bool resolveAutoAudioIfNeeded() const;

	AudioSelectionMode audio_selection_mode_ = AudioSelectionMode::Default;

	mutable bool auto_audio_resolved_ = false;
	mutable ResolvedAudioDevices auto_audio_devices_;

	static QAudioFormat chooseAudioFormat(const QAudioDevice &device) ;
	static QString sampleFormatName(QAudioFormat::SampleFormat format);

	QString rx_device_name_;
	QString tx_device_name_;
	bool debug_ = false;
	bool quiet_ = false;

	QAudioDevice rx_device_;
	QAudioDevice tx_device_;

	QAudioFormat rx_format_;
	QAudioFormat tx_format_;

	int tx_sink_buffer_ms_ = 300;
	int tx_prebuffer_ms_ = 200;
	int tx_jitter_buffer_ms_ = 5000;
	int tx_drain_interval_ms_ = 2;

	bool tx_stream_active_ = false;
	bool tx_output_primed_ = false;

	QTimer tx_drain_timer_;
	QByteArray tx_buffer_;

	qint64 tx_buffer_overflows_ = 0;
	qint64 tx_buffer_underruns_ = 0;
	qint64 tx_drain_ticks_ = 0;
	qint64 tx_write_count_ = 0;
	qint64 tx_overflow_dropped_bytes_ = 0;

	void drainTxAudio();

	std::unique_ptr<QAudioSource> rx_source_;
	std::unique_ptr<QAudioSink> tx_sink_;

	QIODevice *rx_io_ = nullptr;
	QIODevice *tx_io_ = nullptr;
};