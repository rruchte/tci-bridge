#pragma once

#include <QObject>
#include <QString>

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
	virtual void setSplitEnabled(bool enabled) = 0;

	virtual QString mode() const = 0;
	virtual void setMode(const QString &mode) = 0;

	virtual bool ptt() const = 0;
	virtual void setPtt(bool enabled) = 0;

	virtual void setPollingSuspended(bool suspended) = 0;

	virtual bool online() const = 0;

signals:
	void frequencyChanged(qint64 hz);
	void txFrequencyChanged(qint64 hz);
	void splitChanged(bool enabled);
	void modeChanged(QString mode);
	void pttChanged(bool enabled);

	void onlineChanged(bool online);
};