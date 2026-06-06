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

	connect(worker_, &RigctldWorker::modeChanged,
			this, &RigctldBackend::updateMode,
			Qt::QueuedConnection);

	connect(worker_, &RigctldWorker::pttChanged,
			this, &RigctldBackend::updatePtt,
			Qt::QueuedConnection);

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
	if (hz <= 0)
		return;

	if (frequency_hz_ != hz) {
		frequency_hz_ = hz;
		emit frequencyChanged(hz);
	}

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
	if (hz <= 0)
		return;

	if (tx_frequency_hz_ != hz) {
		tx_frequency_hz_ = hz;
		emit txFrequencyChanged(hz);
	}

	emit requestSetTxFrequencyHz(hz);
}

bool RigctldBackend::splitEnabled() const
{
	return split_enabled_;
}

void RigctldBackend::setSplitEnabled(bool enabled)
{
	if (split_enabled_ != enabled) {
		split_enabled_ = enabled;
		emit splitChanged(enabled);
	}

	emit requestSetSplitEnabled(enabled);
}

QString RigctldBackend::mode() const
{
	return mode_;
}

void RigctldBackend::setMode(const QString &mode)
{
	const QString normalized = mode.trimmed();

	if (normalized.isEmpty())
		return;

	if (mode_ != normalized) {
		mode_ = normalized;
		emit modeChanged(normalized);
	}

	emit requestSetMode(normalized);
}

bool RigctldBackend::ptt() const
{
	return ptt_;
}

void RigctldBackend::setPtt(bool enabled)
{
	// Important: this is logical bridge state. Do not block the main/audio
	// thread waiting for rigctld confirmation.
	if (ptt_ != enabled) {
		ptt_ = enabled;
		emit pttChanged(enabled);
	}

	emit requestSetPtt(enabled);
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
		emit onlineChanged(false);
	}
}

void RigctldBackend::updateRadioUsable(bool usable)
{
	const bool oldOnline = online();

	radio_usable_ = usable;

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
	if (frequency_hz_ == hz)
		return;

	frequency_hz_ = hz;
	emit frequencyChanged(hz);
}

void RigctldBackend::updateTxFrequency(qint64 hz)
{
	if (tx_frequency_hz_ == hz)
		return;

	tx_frequency_hz_ = hz;
	emit txFrequencyChanged(hz);
}

void RigctldBackend::updateSplit(bool enabled)
{
	if (split_enabled_ == enabled)
		return;

	split_enabled_ = enabled;
	emit splitChanged(enabled);
}

void RigctldBackend::updateMode(const QString &mode)
{
	if (mode_ == mode)
		return;

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