// SignalHandler.cpp
#include "SignalHandler.h"

#include <QCoreApplication>
#include <QDebug>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

int SignalHandler::sigintFd_[2] = {-1, -1};
int SignalHandler::sigtermFd_[2] = {-1, -1};

SignalHandler::SignalHandler(QObject *parent)
	: QObject(parent)
{
	sigintNotifier_ = new QSocketNotifier(sigintFd_[1], QSocketNotifier::Read, this);
	connect(sigintNotifier_, &QSocketNotifier::activated,
			this, &SignalHandler::handleSigInt);

	sigtermNotifier_ = new QSocketNotifier(sigtermFd_[1], QSocketNotifier::Read, this);
	connect(sigtermNotifier_, &QSocketNotifier::activated,
			this, &SignalHandler::handleSigTerm);
}

SignalHandler::~SignalHandler()
{
	if (sigintFd_[0] >= 0)
		::close(sigintFd_[0]);

	if (sigintFd_[1] >= 0)
		::close(sigintFd_[1]);

	if (sigtermFd_[0] >= 0)
		::close(sigtermFd_[0]);

	if (sigtermFd_[1] >= 0)
		::close(sigtermFd_[1]);
}

bool SignalHandler::install()
{
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigintFd_) != 0)
		return false;

	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigtermFd_) != 0)
		return false;

	struct sigaction intAction {};
	intAction.sa_handler = SignalHandler::signalHandler;
	sigemptyset(&intAction.sa_mask);
	intAction.sa_flags = 0;

	if (::sigaction(SIGINT, &intAction, nullptr) != 0)
		return false;

	struct sigaction termAction {};
	termAction.sa_handler = SignalHandler::signalHandler;
	sigemptyset(&termAction.sa_mask);
	termAction.sa_flags = 0;

	if (::sigaction(SIGTERM, &termAction, nullptr) != 0)
		return false;

	return true;
}

void SignalHandler::signalHandler(int signal)
{
	char c = 1;

	if (signal == SIGINT)
		::write(sigintFd_[0], &c, sizeof(c));
	else if (signal == SIGTERM)
		::write(sigtermFd_[0], &c, sizeof(c));
}

void SignalHandler::handleSigInt()
{
	sigintNotifier_->setEnabled(false);

	char c;
	::read(sigintFd_[1], &c, sizeof(c));

	qInfo() << "SIGINT received; shutting down";

	emit interruptReceived();

	QCoreApplication::quit();

	sigintNotifier_->setEnabled(true);
}

void SignalHandler::handleSigTerm()
{
	sigtermNotifier_->setEnabled(false);

	char c;
	::read(sigtermFd_[1], &c, sizeof(c));

	qInfo() << "SIGTERM received; shutting down";

	emit terminateReceived();

	QCoreApplication::quit();

	sigtermNotifier_->setEnabled(true);
}