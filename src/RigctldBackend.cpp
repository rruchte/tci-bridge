#include "RigctldBackend.h"
#include "RigctldWorker.h"

#include <QDebug>
#include <QMetaObject>

RigctldBackend::RigctldBackend(
	QString host,
	quint16 port,
	int pollMs,
	QObject *parent
)
	: RadioBackend(parent),
	  host_(std::move(host)),
	  port_(port),
	  poll_ms_(qMax(50, pollMs))
{
	setupWorker();

	qInfo() << "Rigctld backend configured:"
			<< host_ << port_
			<< "poll_ms=" << poll_ms_;

	emit requestStart(host_, port_, poll_ms_);
}

RigctldBackend::~RigctldBackend()
{
	if (worker_ && worker_thread_.isRunning()) {
		QMetaObject::invokeMethod(worker_,
								  "stop",
								  Qt::BlockingQueuedConnection);
	}

	worker_thread_.quit();
	worker_thread_.wait(1500);
}

void RigctldBackend::setupWorker()
{
	worker_ = new RigctldWorker;
	worker_->moveToThread(&worker_thread_);

	connect(&worker_thread_, &QThread::finished,
			worker_, &QObject::deleteLater);

	connect(this, &RigctldBackend::requestStart,
			worker_, &RigctldWorker::start,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestStop,
			worker_, &RigctldWorker::stop,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestReadRxControl,
	        worker_, &RigctldWorker::readRxControl, Qt::QueuedConnection);
	connect(this, &RigctldBackend::requestSetRxControl,
	        worker_, &RigctldWorker::setRxControl, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::rxControlChanged, this,
	        [this](RxControl setting, int value, bool known) {
		        auto &cached = rx_controls_[static_cast<int>(setting)];
		        if (known)
			        cached = value;
		        else
			        cached.reset();
		        emit rxControlChanged(setting, value, known);
	        }, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestTxMeterInterval,
	        worker_, &RigctldWorker::setTxMeterInterval, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::txMeterReading,
	        this, &RadioBackend::txMeterReading, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestRxMeterInterval,
	        worker_, &RigctldWorker::setRxMeterInterval, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::rxSignalLevel,
	        this, &RadioBackend::rxSignalLevel, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestFilterEdgesHzEnabled,
	        worker_, &RigctldWorker::setFilterEdgesHzEnabled, Qt::QueuedConnection);
	connect(this, &RigctldBackend::requestReadRxFilterBand,
	        worker_, &RigctldWorker::readRxFilterBand, Qt::QueuedConnection);
	connect(this, &RigctldBackend::requestSetRxFilterBand,
	        worker_, &RigctldWorker::setRxFilterBand, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::rxFilterBandChanged, this, [this](int low, int high) {
		rx_filter_band_ = {low, high};
		emit rxFilterBandChanged(low, high);
	}, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestReadAgc,
	        worker_, &RigctldWorker::readAgc, Qt::QueuedConnection);
	connect(this, &RigctldBackend::requestSetAgcMode,
	        worker_, &RigctldWorker::setAgcMode, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::agcModeChanged, this, [this](const QString &mode) {
		agc_mode_ = mode;
		emit agcModeChanged(mode);
	}, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestReadDrive,
	        worker_, &RigctldWorker::readDrive, Qt::QueuedConnection);
	connect(this, &RigctldBackend::requestSetDrivePercent,
	        worker_, &RigctldWorker::setDrivePercent, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::driveChanged, this, [this](int percent) {
		drive_percent_ = percent;
		emit driveChanged(percent);
	}, Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetFrequencyHz,
			worker_, &RigctldWorker::setFrequencyHz,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetTxFrequencyHz,
			worker_, &RigctldWorker::setTxFrequencyHz,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetSplitEnabled,
			worker_, &RigctldWorker::setSplitEnabled,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetMode,
			worker_, &RigctldWorker::setMode,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetPtt,
			worker_, &RigctldWorker::setPtt,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetPollingSuspended,
			worker_, &RigctldWorker::setPollingSuspended,
			Qt::QueuedConnection);

	connect(this, &RigctldBackend::requestSetDebug,
			worker_, &RigctldWorker::setDebug,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::frequencyChanged,
			this, &RigctldBackend::updateFrequency,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::txFrequencyChanged,
			this, &RigctldBackend::updateTxFrequency,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::splitChanged,
			this, &RigctldBackend::updateSplit,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::splitStateUnavailable, this, [this] {
		split_known_ = false;
		emit effectiveTxFrequencyInvalidated();
	}, Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::modeChanged,
			this, &RigctldBackend::updateMode,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::supportedModesChanged,
			this, &RigctldBackend::updateSupportedModes,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::vfoLimitsChanged,
			this, &RigctldBackend::updateVfoLimits,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::pttChanged,
			this, &RigctldBackend::updatePtt,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::pttCommandFailed,
			this, &RadioBackend::pttCommandFailed, Qt::QueuedConnection);
	connect(worker_, &RigctldWorker::pttCommandSucceeded,
			this, &RadioBackend::pttCommandSucceeded, Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::connectedChanged,
			this, [](bool connected) {
				qInfo() << "[RigctldBackend] connected="
						<< connected;
			},
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::error,
			this, [](const QString &message) {
				qWarning().noquote()
					<< "[RigctldBackend]" << message;
			},
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::slowCall,
			this, [](const QString &operation, qint64 elapsedMs) {
				qWarning() << "[RigctldBackend] slow call:"
						   << operation
						   << elapsedMs << "ms";
			},
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::connectedChanged,
		this, &RigctldBackend::updateConnected);

	connect(worker_, &RigctldWorker::radioUsableChanged,
			this, &RigctldBackend::updateRadioUsable);

	worker_thread_.setObjectName(QStringLiteral("RigctldWorkerThread"));
	worker_thread_.start();
}

qint64 RigctldBackend::frequencyHz() const
{
	return frequency_hz_;
}

void RigctldBackend::setFrequencyHz(qint64 hz)
{
	emit requestSetFrequencyHz(hz);
}

qint64 RigctldBackend::txFrequencyHz() const
{
	if (tx_frequency_hz_ > 0)
		return tx_frequency_hz_;

	return frequencyHz();
}

void RigctldBackend::setTxFrequencyHz(qint64 hz)
{
	emit requestSetTxFrequencyHz(hz);
}

bool RigctldBackend::splitEnabled() const
{
	return split_enabled_;
}

qint64 RigctldBackend::effectiveTxFrequencyHz() const
{
	if (!online() || !split_known_)
		return 0;
	const qint64 base = split_enabled_ ? tx_frequency_hz_ : frequency_hz_;
	if (base <= 0)
		return 0;
	const bool xitEnabled = rxControlValue(RxControl::XitEnabled).value_or(0);
	const auto xitOffset = rxControlValue(RxControl::XitOffset);
	if (xitEnabled && !xitOffset)
		return 0;
	return base + (xitEnabled ? xitOffset.value_or(0) : 0);
}

void RigctldBackend::setTxMeterInterval(int milliseconds)
{
	emit requestTxMeterInterval(milliseconds);
}

void RigctldBackend::setRxMeterInterval(int milliseconds)
{
	emit requestRxMeterInterval(milliseconds);
}

void RigctldBackend::setFilterEdgesHzEnabled(bool enabled)
{
	emit requestFilterEdgesHzEnabled(enabled);
}

QPair<int, int> RigctldBackend::rxFilterBand() const
{
	return online() ? rx_filter_band_ : QPair<int, int>{};
}

void RigctldBackend::requestRxFilterBand()
{
	if (online())
		emit requestReadRxFilterBand();
}

void RigctldBackend::setRxFilterBand(int low, int high)
{
	if (online())
		emit requestSetRxFilterBand(low, high);
}

QString RigctldBackend::agcMode() const
{
	return online() ? agc_mode_ : QString();
}

void RigctldBackend::requestAgcReadback()
{
	if (online())
		emit requestReadAgc();
}

void RigctldBackend::setAgcMode(const QString &mode)
{
	if (online())
		emit requestSetAgcMode(mode);
}

int RigctldBackend::drivePercent() const
{
	return online() ? drive_percent_ : -1;
}

void RigctldBackend::requestDriveReadback()
{
	if (online())
		emit requestReadDrive();
	else
		qWarning() << "Cannot read DRIVE while CAT is offline";
}

void RigctldBackend::setDrivePercent(int percent)
{
	if (online())
		emit requestSetDrivePercent(percent);
}

std::optional<int> RigctldBackend::rxControlValue(RxControl setting) const
{
	if (!online())
		return {};
	return rx_controls_[static_cast<int>(setting)];
}

void RigctldBackend::requestRxControl(RxControl setting)
{
	if (online())
		emit requestReadRxControl(setting);
}

void RigctldBackend::setRxControl(RxControl setting, int value)
{
	if (online())
		emit requestSetRxControl(setting, value);
}

void RigctldBackend::setSplitEnabled(bool enabled)
{
	emit requestSetSplitEnabled(enabled);
}

QString RigctldBackend::mode() const
{
	return mode_;
}

void RigctldBackend::setMode(const QString &mode)
{
	emit requestSetMode(mode);
}

bool RigctldBackend::ptt() const
{
	return ptt_;
}

void RigctldBackend::setPtt(bool enabled)
{
	emit requestSetPtt(enabled);
}

QPair<qint64, qint64> RigctldBackend::vfoLimits() const
{
	return vfo_limits_;
}

void RigctldBackend::updateVfoLimits(qint64 minHz, qint64 maxHz)
{
	const QPair<qint64, qint64> limits{minHz, maxHz};
	if (vfo_limits_ == limits)
		return;
	vfo_limits_ = limits;
	emit vfoLimitsChanged(minHz, maxHz);
}

QStringList RigctldBackend::supportedModes() const
{
	return supported_modes_;
}

void RigctldBackend::updateSupportedModes(const QStringList &modes)
{
	if (supported_modes_ == modes)
		return;
	supported_modes_ = modes;
	emit supportedModesChanged(modes);
}

bool RigctldBackend::online() const
{
	return connected_ && radio_usable_;
}

void RigctldBackend::updateConnected(bool connected)
{
	if (connected_ == connected) return;

	connected_ = connected;

	if (!connected_) {
		radio_usable_ = false;
		split_known_ = false;
		tx_frequency_hz_ = 0;
		rx_controls_ = {};
		drive_percent_ = -1;
		agc_mode_.clear();
		rx_filter_band_ = {};
		updateSupportedModes({});
		updateVfoLimits(0, 0);
		emit onlineChanged(false);
	}
}

void RigctldBackend::updateRadioUsable(bool usable)
{
	const bool oldOnline = online();

	radio_usable_ = usable;
	if (!usable) {
		rx_controls_ = {};
		drive_percent_ = -1;
		agc_mode_.clear();
		rx_filter_band_ = {};
		split_known_ = false;
		tx_frequency_hz_ = 0;
	}

	const bool newOnline = online();
	if (oldOnline != newOnline) {
		emit onlineChanged(newOnline);
	}
}

void RigctldBackend::setPollingSuspended(bool suspended)
{
	if (polling_suspended_ == suspended)
		return;

	polling_suspended_ = suspended;

	qInfo() << "[RigctldBackend] polling"
			<< (polling_suspended_ ? "suspended" : "resumed");

	emit requestSetPollingSuspended(suspended);
}

void RigctldBackend::setDebug(bool enabled)
{
	debug_ = enabled;
	emit requestSetDebug(enabled);
}

void RigctldBackend::updateFrequency(qint64 hz)
{
	frequency_hz_ = hz;
	emit frequencyChanged(hz);
}

void RigctldBackend::updateTxFrequency(qint64 hz)
{
	tx_frequency_hz_ = hz;
	emit txFrequencyChanged(hz);
}

void RigctldBackend::updateSplit(bool enabled)
{
	split_enabled_ = enabled;
	split_known_ = true;
	emit splitChanged(enabled);
}

void RigctldBackend::updateMode(const QString &mode)
{
	mode_ = mode;
	emit modeChanged(mode);
}

void RigctldBackend::updatePtt(bool enabled)
{
	if (ptt_ == enabled)
		return;

	ptt_ = enabled;
	emit pttChanged(enabled);
}