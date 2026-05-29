// SignalHandler.h
#pragma once

#include <QObject>
#include <QSocketNotifier>

class SignalHandler final : public QObject
{
	Q_OBJECT

public:
	explicit SignalHandler(QObject *parent = nullptr);
	~SignalHandler() override;

	static bool install();

	signals:
		void interruptReceived();
	void terminateReceived();

private slots:
	void handleSigInt();
	void handleSigTerm();

private:
	static void signalHandler(int signal);

	static int sigintFd_[2];
	static int sigtermFd_[2];

	QSocketNotifier *sigintNotifier_ = nullptr;
	QSocketNotifier *sigtermNotifier_ = nullptr;
};