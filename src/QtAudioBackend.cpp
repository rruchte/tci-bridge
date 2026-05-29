#include "QtAudioBackend.h"
#include "PcmConvert.h"

#include <QAudio>
#include <QDebug>
#include <QMediaDevices>

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
                qInfo() << "RX audio state changed:"
                        << state
                        << "error=" << rx_source_->error();
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

    tx_sink_ = std::make_unique<QAudioSink>(tx_device_, tx_format_);
    tx_sink_->setBufferSize(8192);

    connect(tx_sink_.get(), &QAudioSink::stateChanged,
            this, [this](QAudio::State state) {
                qInfo() << "TX audio state changed:"
                        << state
                        << "error=" << tx_sink_->error();
            });

    tx_io_ = tx_sink_->start();

    if (!tx_io_) {
        qWarning() << "Failed to start TX audio sink:"
                   << tx_device_.description()
                   << "error=" << tx_sink_->error();
        tx_sink_.reset();
        return false;
    }

    qInfo() << "Started TX audio:"
            << tx_device_.description()
            << tx_format_.sampleRate() << "Hz"
            << tx_format_.channelCount() << "ch"
            << sampleFormatName(tx_format_.sampleFormat());

    return true;
}

void QtAudioBackend::stopTx()
{
    if (tx_sink_) {
        tx_sink_->stop();
        tx_sink_.reset();
    }

    tx_io_ = nullptr;
}

void QtAudioBackend::writeTxAudio(const QByteArray &pcm)
{
	if (!tx_io_ || pcm.isEmpty())
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

	const qint64 written = tx_io_->write(converted);

	if (debug_) {
		qInfo() << "Audio TX write:"
				<< written
				<< "of"
				<< converted.size()
				<< "bytes converted from"
				<< pcm.size()
				<< "bytes TCI mono int16";
	}

	if (written < 0) {
		qWarning() << "TX audio write failed";
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