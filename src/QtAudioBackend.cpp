#include "QtAudioBackend.h"
#include "PcmConvert.h"

#include <QAudio>
#include <QDebug>
#include <QMediaDevices>

QtAudioBackend::~QtAudioBackend() = default;

QtAudioBackend::QtAudioBackend(QObject *parent)
	: AudioBackend(parent)
{
}

void QtAudioBackend::setRxDeviceName(const QString &name)
{
	rx_device_name_ = name.trimmed();
}

void QtAudioBackend::setTxDeviceName(const QString &name)
{
	tx_device_name_ = name.trimmed();
}

void QtAudioBackend::setDebug(bool enabled)
{
	debug_ = enabled;
}

void QtAudioBackend::setQuiet(bool enabled)
{
	quiet_ = enabled;
}

void QtAudioBackend::setTxSinkBufferMs(int ms)
{
	tx_sink_buffer_ms_ = qMax(20, ms);
}

void QtAudioBackend::setTxPrebufferMs(int ms)
{
	tx_prebuffer_ms_ = qMax(0, ms);
}

void QtAudioBackend::setTxJitterBufferMs(int ms)
{
	tx_jitter_buffer_ms_ = qMax(100, ms);
}

void QtAudioBackend::setTxDrainIntervalMs(int ms)
{
	tx_drain_interval_ms_ = qMax(1, ms);

	if (tx_drain_timer_.isActive())
		tx_drain_timer_.start(tx_drain_interval_ms_);
}

bool QtAudioBackend::startRx()
{
	stopRx();

	QAudioDevice selected;

	if (!chooseRxDevice(&selected)) {
		qWarning() << "No suitable RX audio input device found";
		return false;
	}

	rx_device_ = selected;
	rx_format_ = chooseAudioFormat(rx_device_);

	if (!rx_device_.isFormatSupported(rx_format_)) {
		qWarning() << "Chosen RX format is not supported by device:"
				   << rx_device_.description()
				   << rx_format_;
		return false;
	}

	rx_source_ = std::make_unique<QAudioSource>(rx_device_, rx_format_);
	rx_source_->setBufferSize(8192);

	connect(rx_source_.get(), &QAudioSource::stateChanged,
			this, [this](QAudio::State state) {
				if (!quiet_)
				{
					qInfo() << "RX audio state changed:"
							<< state
							<< "error=" << rx_source_->error();
				}
			});

	rx_io_ = rx_source_->start();

	if (!rx_io_) {
		qWarning() << "Failed to start RX audio source:"
				   << rx_device_.description()
				   << "error=" << rx_source_->error();
		rx_source_.reset();
		return false;
	}

	connect(rx_io_, &QIODevice::readyRead,
			this, &QtAudioBackend::onRxReadyRead);

	qInfo() << "Started RX audio:"
			<< rx_device_.description()
			<< rx_format_.sampleRate() << "Hz"
			<< rx_format_.channelCount() << "ch"
			<< sampleFormatName(rx_format_.sampleFormat());

	return true;
}

void QtAudioBackend::stopRx()
{
	if (rx_source_) {
		rx_source_->stop();
		rx_source_.reset();
	}

	rx_io_ = nullptr;
}

bool QtAudioBackend::startTx()
{
	stopTx();

	QAudioDevice selected;

	if (!chooseTxDevice(&selected)) {
		qWarning() << "No suitable TX audio output device found";
		return false;
	}

	tx_device_ = selected;
	tx_format_ = chooseAudioFormat(tx_device_);

	if (!tx_device_.isFormatSupported(tx_format_)) {
		qWarning() << "Chosen TX format is not supported by device:"
				   << tx_device_.description()
				   << tx_format_;
		return false;
	}

	tx_buffer_.clear();
	tx_buffer_overflows_ = 0;
	tx_buffer_underruns_ = 0;
	tx_drain_ticks_ = 0;
	tx_output_primed_ = false;

	tx_sink_ = std::make_unique<QAudioSink>(tx_device_, tx_format_);
	tx_sink_->setBufferSize(
		tx_format_.bytesForDuration(tx_sink_buffer_ms_ * 1000)
	);

	connect(tx_sink_.get(), &QAudioSink::stateChanged,
		this, [this](QAudio::State state) {
			if (state == QAudio::IdleState &&
				tx_stream_active_ &&
				tx_output_primed_ &&
				tx_buffer_.isEmpty()) {
				++tx_buffer_underruns_;
				tx_output_primed_ = false;
			}
			if (!quiet_)
			{
				qInfo() << "TX audio state changed:"
						<< state
						<< "error=" << tx_sink_->error()
						<< "buffered=" << tx_buffer_.size()
						<< "bytesFree=" << tx_sink_->bytesFree()
						<< "streamActive=" << tx_stream_active_
						<< "primed=" << tx_output_primed_
						<< "underruns=" << tx_buffer_underruns_
						<< "overflows=" << tx_buffer_overflows_;
			}
		});

	tx_io_ = tx_sink_->start();

	if (!tx_io_) {
		qWarning() << "Failed to start TX audio sink:"
				   << tx_device_.description()
				   << "error=" << tx_sink_->error();
		tx_sink_.reset();
		return false;
	}

	tx_drain_timer_.setTimerType(Qt::PreciseTimer);
	connect(&tx_drain_timer_, &QTimer::timeout,
			this, &QtAudioBackend::drainTxAudio,
			Qt::UniqueConnection);

	tx_drain_timer_.start(tx_drain_interval_ms_);

	qInfo() << "Started TX audio:"
		<< tx_device_.description()
		<< tx_format_.sampleRate() << "Hz"
		<< tx_format_.channelCount() << "ch"
		<< sampleFormatName(tx_format_.sampleFormat())
		<< "sinkBufferMs=" << tx_sink_buffer_ms_
		<< "sinkBufferBytes=" << tx_sink_->bufferSize()
		<< "prebufferMs=" << tx_prebuffer_ms_
		<< "jitterBufferMs=" << tx_jitter_buffer_ms_
		<< "drainIntervalMs=" << tx_drain_interval_ms_;

	return true;
}

void QtAudioBackend::stopTx()
{
	tx_drain_timer_.stop();

	if (tx_sink_) {
		tx_sink_->stop();
		tx_sink_.reset();
	}

	tx_io_ = nullptr;
	tx_buffer_.clear();
	tx_output_primed_ = false;
}

void QtAudioBackend::beginTxAudioStream()
{
	tx_buffer_.clear();

	tx_stream_active_ = true;
	tx_output_primed_ = false;

	tx_buffer_overflows_ = 0;
	tx_buffer_underruns_ = 0;
	tx_drain_ticks_ = 0;
	tx_write_count_ = 0;
	tx_overflow_dropped_bytes_ = 0;

	qInfo() << "TX audio stream begin";
}

void QtAudioBackend::endTxAudioStream()
{
	const qsizetype remaining = tx_buffer_.size();

	tx_stream_active_ = false;
	tx_output_primed_ = false;

	tx_buffer_.clear();

	qInfo() << "TX audio stream end"
			<< "discardedBuffered=" << remaining
			<< "underruns=" << tx_buffer_underruns_
			<< "overflows=" << tx_buffer_overflows_
			<< "droppedBytes=" << tx_overflow_dropped_bytes_;
}

void QtAudioBackend::writeTxAudio(const QByteArray &pcm)
{
	if (!tx_sink_ || !tx_io_ || pcm.isEmpty())
		return;

	const QByteArray converted = PcmConvert::monoInt16ToFormat(
		pcm,
		tx_format_
	);

	if (converted.isEmpty()) {
		qWarning() << "TX audio conversion produced no data for format:"
				   << PcmConvert::describeFormat(tx_format_);
		return;
	}

	tx_buffer_.append(converted);
	++tx_write_count_;

	const qsizetype maxBufferedBytes = tx_format_.bytesForDuration(tx_jitter_buffer_ms_ * 1000);

	if (tx_buffer_.size() > maxBufferedBytes) {
		const qsizetype drop = tx_buffer_.size() - maxBufferedBytes;
		tx_buffer_.remove(0, drop);

		++tx_buffer_overflows_;
		tx_overflow_dropped_bytes_ += drop;

		if (tx_buffer_overflows_ == 1 || !(tx_buffer_overflows_ % 100)) {
			qWarning() << "TX push buffer overflow"
					   << "count=" << tx_buffer_overflows_
					   << "lastDrop=" << drop
					   << "totalDropped=" << tx_overflow_dropped_bytes_
					   << "buffered=" << tx_buffer_.size();
		}
	}

	drainTxAudio();

	if (debug_ && !(tx_write_count_ % 500)) {
		qInfo() << "Audio TX queued"
				<< "writes=" << tx_write_count_
				<< "appended=" << converted.size()
				<< "fromTci=" << pcm.size()
				<< "buffered=" << tx_buffer_.size()
				<< "bytesFree=" << tx_sink_->bytesFree()
				<< "state=" << tx_sink_->state()
				<< "primed=" << tx_output_primed_
				<< "streamActive=" << tx_stream_active_
				<< "underruns=" << tx_buffer_underruns_
				<< "overflows=" << tx_buffer_overflows_;
	}
}

void QtAudioBackend::drainTxAudio()
{
	if (!tx_sink_ || !tx_io_)
		return;

	++tx_drain_ticks_;

	const qsizetype prebufferBytes = tx_format_.bytesForDuration(tx_prebuffer_ms_ * 1000);

	if (!tx_output_primed_) {
		if (tx_buffer_.size() < prebufferBytes)
			return;

		tx_output_primed_ = true;
		if (!quiet_)
		{
			qInfo() << "TX push buffer primed"
					<< "buffered=" << tx_buffer_.size()
					<< "prebuffer=" << prebufferBytes;
		}
	}

	if (tx_sink_->state() == QAudio::SuspendedState)
		tx_sink_->resume();

	if (tx_buffer_.isEmpty()) {
		if (tx_stream_active_ &&
			tx_output_primed_ &&
			tx_sink_->state() == QAudio::IdleState) {
			++tx_buffer_underruns_;
			tx_output_primed_ = false;
			}

		return;
	}

	const qint64 freeBytes = tx_sink_->bytesFree();

	if (freeBytes <= 0)
		return;

	const qint64 toWrite =
		qMin<qint64>(freeBytes, tx_buffer_.size());

	const qint64 written =
		tx_io_->write(tx_buffer_.constData(), toWrite);

	if (written < 0) {
		qWarning() << "TX audio write failed";
		return;
	}

	if (written > 0)
		tx_buffer_.remove(0, written);

	if (tx_sink_->state() == QAudio::IdleState && !tx_buffer_.isEmpty())
		tx_sink_->resume();

	if (debug_ && !(tx_drain_ticks_ % 1000)) {
		qInfo() << "Audio TX drain"
				<< "buffered=" << tx_buffer_.size()
				<< "bytesFree=" << tx_sink_->bytesFree()
				<< "state=" << tx_sink_->state()
				<< "primed=" << tx_output_primed_
				<< "streamActive=" << tx_stream_active_
				<< "underruns=" << tx_buffer_underruns_
				<< "overflows=" << tx_buffer_overflows_;
	}
}

int QtAudioBackend::sampleRate() const
{
	return 48000;
}

int QtAudioBackend::channelCount() const
{
	return 1;
}

void QtAudioBackend::onRxReadyRead()
{
	if (!rx_io_)
		return;

	const QByteArray pcm = rx_io_->readAll();

	if (pcm.isEmpty())
		return;

	if (debug_) {
		qInfo() << "Audio RX chunk:"
				<< pcm.size()
				<< "bytes format="
				<< QStringLiteral("%1Hz %2ch %3")
					   .arg(rx_format_.sampleRate())
					   .arg(rx_format_.channelCount())
					   .arg(sampleFormatName(rx_format_.sampleFormat()));
	}

	emit rxAudioFrame(pcm);
}

bool QtAudioBackend::chooseRxDevice(QAudioDevice *selected) const
{
	const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();

	if (inputs.isEmpty())
		return false;

	if (!rx_device_name_.isEmpty()) {
		for (const QAudioDevice &device : inputs) {
			if (device.description() == rx_device_name_) {
				*selected = device;
				return true;
			}
		}

		for (const QAudioDevice &device : inputs) {
			if (device.description().contains(rx_device_name_, Qt::CaseInsensitive)) {
				*selected = device;
				return true;
			}
		}

		qWarning() << "Requested RX audio input was not found:"
				   << rx_device_name_;
	}

	*selected = QMediaDevices::defaultAudioInput();
	return !selected->isNull();
}

bool QtAudioBackend::chooseTxDevice(QAudioDevice *selected) const
{
	const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();

	if (outputs.isEmpty())
		return false;

	if (!tx_device_name_.isEmpty()) {
		for (const QAudioDevice &device : outputs) {
			if (device.description() == tx_device_name_) {
				*selected = device;
				return true;
			}
		}

		for (const QAudioDevice &device : outputs) {
			if (device.description().contains(tx_device_name_, Qt::CaseInsensitive)) {
				*selected = device;
				return true;
			}
		}

		qWarning() << "Requested TX audio output was not found:"
				   << tx_device_name_;
	}

	*selected = QMediaDevices::defaultAudioOutput();
	return !selected->isNull();
}

QAudioFormat QtAudioBackend::chooseAudioFormat(const QAudioDevice &device) const
{
	QAudioFormat format;

	format.setSampleRate(48000);
	format.setChannelCount(1);
	format.setSampleFormat(QAudioFormat::Int16);

	if (device.isFormatSupported(format))
		return format;

	format.setChannelCount(2);

	if (device.isFormatSupported(format))
		return format;

	const QAudioFormat preferred = device.preferredFormat();

	qWarning() << "Falling back to preferred audio format:"
			   << preferred;

	return preferred;
}

void QtAudioBackend::listAudioDevices()
{
	qInfo().noquote() << "Audio input devices:";

	const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();

	for (const QAudioDevice &device : inputs) {
		const QAudioFormat preferred = device.preferredFormat();

		qInfo().noquote()
			<< QStringLiteral("  INPUT: \"%1\" preferred=%2Hz %3ch %4")
				   .arg(device.description())
				   .arg(preferred.sampleRate())
				   .arg(preferred.channelCount())
				   .arg(sampleFormatName(preferred.sampleFormat()));
	}

	qInfo().noquote() << "";
	qInfo().noquote() << "Audio output devices:";

	const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();

	for (const QAudioDevice &device : outputs) {
		const QAudioFormat preferred = device.preferredFormat();

		qInfo().noquote()
			<< QStringLiteral("  OUTPUT: \"%1\" preferred=%2Hz %3ch %4")
				   .arg(device.description())
				   .arg(preferred.sampleRate())
				   .arg(preferred.channelCount())
				   .arg(sampleFormatName(preferred.sampleFormat()));
	}
}

QAudioFormat QtAudioBackend::rxFormat() const
{
	return rx_format_;
}

QAudioFormat QtAudioBackend::txFormat() const
{
	return tx_format_;
}

QString QtAudioBackend::sampleFormatName(QAudioFormat::SampleFormat format)
{
	switch (format) {
		case QAudioFormat::UInt8:
			return "UInt8";
		case QAudioFormat::Int16:
			return "Int16";
		case QAudioFormat::Int32:
			return "Int32";
		case QAudioFormat::Float:
			return "Float";
		case QAudioFormat::Unknown:
		default:
			return "Unknown";
	}
}