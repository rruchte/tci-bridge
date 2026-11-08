#pragma once

#include "RxControl.h"
#include "TxMeterReading.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QPair>

class RadioBackend : public QObject
{
	Q_OBJECT

public:
	explicit RadioBackend(QObject *parent = nullptr) : QObject(parent) {}
	~RadioBackend() override = default;

	virtual qint64 frequencyHz() const = 0;
	virtual void setFrequencyHz(qint64 hz) = 0;

	virtual qint64 txFrequencyHz() const = 0;
	virtual void setTxFrequencyHz(qint64 hz) = 0;

	virtual bool splitEnabled() const = 0;
	// Zero denotes an unknown transmit frequency.
	virtual qint64 effectiveTxFrequencyHz() const
	{
		return online() ? (splitEnabled() ? txFrequencyHz() : frequencyHz()) : 0;
	}
	virtual void setSplitEnabled(bool enabled) = 0;

	virtual QString mode() const = 0;
	virtual void setMode(const QString &mode) = 0;

	virtual bool ptt() const = 0;
	virtual void setPtt(bool enabled) = 0;

	virtual void setPollingSuspended(bool suspended) = 0;

	virtual bool online() const = 0;

	virtual void setRxMeterInterval(int) {}
	virtual bool supportsCompleteTxMeter() const { return false; }
	virtual void setTxMeterInterval(int) {}
	virtual QPair<int, int> rxFilterBand() const { return {}; }
	virtual void requestRxFilterBand() {}
	virtual void setRxFilterBand(int, int) {}

	virtual QString agcMode() const { return {}; }
	virtual void requestAgcReadback() {}
	virtual void setAgcMode(const QString &) {}

	virtual int drivePercent() const { return -1; }
	virtual void requestDriveReadback() {}
	virtual void setDrivePercent(int) {}

	virtual std::optional<int> rxControlValue(RxControl) const { return {}; }
	virtual void requestRxControl(RxControl) {}
	virtual void setRxControl(RxControl, int) {}


	// Empty means capabilities could not be determined. Never synthesize
	// supported modes from the current mode alone.
	virtual QStringList supportedModes() const { return {}; }
	virtual QPair<qint64, qint64> vfoLimits() const { return {0, 0}; }

signals:
	void rxControlChanged(RxControl setting, int value, bool known);
	void rxSignalLevel(int dbm);
	void txMeterReading(const TxMeterReading &reading);
	void rxFilterBandChanged(int low, int high);
	void agcModeChanged(const QString &mode);
	void driveChanged(int percent);
	void frequencyChanged(qint64 hz);
	void txFrequencyChanged(qint64 hz);
	void splitChanged(bool enabled);
	void effectiveTxFrequencyInvalidated();
	void modeChanged(QString mode);
	void pttChanged(bool enabled);
	void pttCommandFailed(bool requested);
	void pttCommandSucceeded(bool requested);

	void onlineChanged(bool online);
	void supportedModesChanged(QStringList modes);
	void vfoLimitsChanged(qint64 minHz, qint64 maxHz);
};