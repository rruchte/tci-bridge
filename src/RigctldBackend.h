#pragma once

#include "RadioBackend.h"

#include <QTcpSocket>
#include <QTimer>

class RigctldBackend final : public RadioBackend
{
	Q_OBJECT

public:
	explicit RigctldBackend(
		QString host,
		quint16 port,
		int pollMs = 250,
		QObject *parent = nullptr
	);

	qint64 frequencyHz() const override;
	void setFrequencyHz(qint64 hz) override;

	QString mode() const override;
	void setMode(const QString &mode) override;

	bool ptt() const override;
	void setPtt(bool enabled) override;
	void setDebug(bool enabled);

private slots:
	void poll();

private:
	QString transact(const QString &command, int timeoutMs = 1000);
	QStringList transactLines(const QString &command, int timeoutMs = 1000);

	void pollFrequency();
	void pollMode();
	void pollPtt();

	static QString normalizeModeForRigctld(const QString &mode);
	static QString normalizeModeFromRigctld(const QString &mode);

private:
	QString host_;
	quint16 port_ = 4532;
	int poll_ms_ = 250;

	mutable qint64 frequency_hz_ = 0;
	mutable QString mode_ = "USB";
	mutable bool ptt_ = false;
	bool debug_ = false;

	QTimer poll_timer_;
};