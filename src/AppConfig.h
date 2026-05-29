#pragma once

#include <QString>

struct AppConfig
{
	QString serverBind = "0.0.0.0";
	quint16 serverPort = 40001;

	QString radioBackend = "null";
	QString rigctldHost = "127.0.0.1";
	quint16 rigctldPort = 4532;
	int pollMs = 250;
	bool rigctldDebug = false;

	QString audioRxDevice;
	QString audioTxDevice;
	bool audioDebug = false;

	bool txAudioKeysPtt = false;

	static AppConfig defaults();
	static bool loadYamlFile(const QString &path, AppConfig *config, QString *error);
};