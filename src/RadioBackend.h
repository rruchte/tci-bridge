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

	virtual QString mode() const = 0;
	virtual void setMode(const QString &mode) = 0;

	virtual bool ptt() const = 0;
	virtual void setPtt(bool enabled) = 0;

	signals:
		void frequencyChanged(qint64 hz);
	void modeChanged(QString mode);
	void pttChanged(bool enabled);
};