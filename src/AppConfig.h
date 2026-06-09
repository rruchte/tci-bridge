#pragma once

#include <QString>

struct AppConfig
{
	QString serverBind = "127.0.0.1";
	quint16 serverPort = 40001;
	bool serverDebug = false;

	QString radioBackend = "null";
	QString rigctldHost = "127.0.0.1";
	quint16 rigctldPort = 4532;
	int pollMs = 250;
	bool rigctldDebug = false;

	QString audioMode = "default";
	QString audioRxDevice;
	QString audioTxDevice;
	bool audioDebug = false;

	int audioTxSinkBufferMs = 300;
	int audioTxPrebufferMs = 200;
	int audioTxJitterBufferMs = 5000;
	int audioTxDrainIntervalMs = 2;

	bool enableTransmit = false;
	bool txAudioKeysPtt = false;
	int maxTxMs = 30000;
	bool unkeyOnDisconnect = true;

	bool quiet = false;
	bool logStartupConfig = true;
	bool logTxTiming = false;

	static AppConfig defaults();

	static bool loadYamlFile(const QString &path, AppConfig *config, QString *error);

	bool normalizeAndValidate(QString *error);
};