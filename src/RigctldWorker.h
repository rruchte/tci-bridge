#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

class RigctldWorker final : public QObject
{
	Q_OBJECT

public:
	explicit RigctldWorker(QObject *parent = nullptr);
	~RigctldWorker() override;

public slots:
	void start(QString host, quint16 port, int pollMs);
	void stop();

	void setFrequencyHz(qint64 hz);
	void setTxFrequencyHz(qint64 hz);
	void setSplitEnabled(bool enabled);
	void setMode(QString mode);
	void setPtt(bool enabled);

	void setPollingSuspended(bool suspended);
	void setDebug(bool enabled);

	void poll();

signals:
	void frequencyChanged(qint64 hz);
	void txFrequencyChanged(qint64 hz);
	void splitChanged(bool enabled);
	void modeChanged(QString mode);
	void pttChanged(bool enabled);

	void connectedChanged(bool connected);
	void error(QString message);
	void slowCall(QString operation, qint64 elapsedMs);

	void radioUsableChanged(bool usable);

private:
	bool ensureConnected();

	QString transact(const QString &command,
					 int timeoutMs,
					 const QString &operation);

	QStringList transactLines(const QString &command,
							  int timeoutMs,
							  const QString &operation);

	bool transactRprt(const QString &command,
					  int timeoutMs,
					  const QString &operation);

	bool transactOptional(const QString &command,
						  QString &response,
						  const QString &description);

	bool pollFrequency();
	bool pollMode();
	bool pollPtt();

	void pollTxFrequencyBestEffort();
	void pollSplitBestEffort();
	void pollOptionalStateOnce();

	void notePollSuccess();
	void notePollFailure(const QString &operation, const QString &response = QString());
	void setRadioUsable(bool usable, const QString &reason = QString());
	void forceReconnect(const QString &reason);

	static QString normalizeModeForRigctld(const QString &mode);
	static QString normalizeModeFromRigctld(const QString &mode);

private:
	QString host_;
	quint16 port_ = 4532;
	int poll_ms_ = 250;

	QTcpSocket *socket_ = nullptr;
	QTimer *poll_timer_ = nullptr;

	bool connected_ = false;
	bool radio_usable_ = false;
	int consecutive_poll_failures_ = 0;
	int max_consecutive_poll_failures_ = 3;
	
	bool debug_ = false;
	bool polling_suspended_ = false;

	bool optional_state_polled_ = false;
	bool tx_freq_query_works_ = true;
	bool split_query_works_ = true;
	bool split_set_works_ = true;

	qint64 frequency_hz_ = 0;
	qint64 tx_frequency_hz_ = 0;
	bool split_enabled_ = false;
	QString mode_ = QStringLiteral("USB");
	bool ptt_ = false;
};