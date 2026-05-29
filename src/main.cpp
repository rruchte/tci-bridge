#include <QCoreApplication>
#include <QCommandLineParser>

#include <QByteArray>
#include <QLoggingCategory>
#include <QMessageLogContext>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "AppConfig.h"
#include "NullRadioBackend.h"
#include "PcmConvert.h"
#include "QtAudioBackend.h"
#include "RigctldBackend.h"
#include "SignalHandler.h"
#include "TciServer.h"
#include "Version.h"

namespace
{
    bool parsePort(const QString &value, quint16 *out, const QString &name)
    {
        bool ok = false;
        const quint16 port = value.toUShort(&ok);

        if (!ok || port == 0) {
            qCritical() << "Invalid" << name << "port:" << value;
            return false;
        }

        *out = port;
        return true;
    }

    bool parseIntMin(const QString &value, int minValue, int *out, const QString &name)
    {
        bool ok = false;
        const int parsed = value.toInt(&ok);

        if (!ok || parsed < minValue) {
            qCritical() << "Invalid" << name << ":" << value
                        << "minimum=" << minValue;
            return false;
        }

        *out = parsed;
        return true;
    }

    void logConfig(const AppConfig &config)
    {
        qInfo().noquote() << "Effective configuration:";
        qInfo().noquote() << QStringLiteral("  server.bind=%1").arg(config.serverBind);
        qInfo().noquote() << QStringLiteral("  server.port=%1").arg(config.serverPort);
    	qInfo().noquote() << QStringLiteral("  server.debug=%1").arg(config.serverDebug ? "true" : "false");
        qInfo().noquote() << QStringLiteral("  radio.backend=%1").arg(config.radioBackend);
        qInfo().noquote() << QStringLiteral("  radio.rigctld_host=%1").arg(config.rigctldHost);
        qInfo().noquote() << QStringLiteral("  radio.rigctld_port=%1").arg(config.rigctldPort);
        qInfo().noquote() << QStringLiteral("  radio.poll_ms=%1").arg(config.pollMs);
        qInfo().noquote() << QStringLiteral("  radio.debug=%1").arg(config.rigctldDebug ? "true" : "false");
        qInfo().noquote() << QStringLiteral("  audio.rx_device=\"%1\"").arg(config.audioRxDevice);
        qInfo().noquote() << QStringLiteral("  audio.tx_device=\"%1\"").arg(config.audioTxDevice);
        qInfo().noquote() << QStringLiteral("  audio.debug=%1").arg(config.audioDebug ? "true" : "false");
    	qInfo().noquote() << QStringLiteral("  audio.tx_sink_buffer_ms=%1").arg(config.audioTxSinkBufferMs);
    	qInfo().noquote() << QStringLiteral("  audio.tx_prebuffer_ms=%1").arg(config.audioTxPrebufferMs);
    	qInfo().noquote() << QStringLiteral("  audio.tx_jitter_buffer_ms=%1").arg(config.audioTxJitterBufferMs);
    	qInfo().noquote() << QStringLiteral("  audio.tx_drain_interval_ms=%1").arg(config.audioTxDrainIntervalMs);
    	qInfo().noquote() << QStringLiteral("  ptt.enable_transmit=%1").arg(config.enableTransmit ? "true" : "false");
    	qInfo().noquote() << QStringLiteral("  ptt.tx_audio_keys_ptt=%1").arg(config.txAudioKeysPtt ? "true" : "false");
    	qInfo().noquote() << QStringLiteral("  ptt.max_tx_ms=%1").arg(config.maxTxMs);
    	qInfo().noquote() << QStringLiteral("  ptt.unkey_on_disconnect=%1").arg(config.unkeyOnDisconnect ? "true" : "false");
    }

	bool validateRuntimeConfig(const AppConfig &config)
    {
    	if (config.audioTxPrebufferMs >= config.audioTxJitterBufferMs) {
    		qCritical() << "Invalid audio buffering:"
						<< "audio.tx_prebuffer_ms must be less than audio.tx_jitter_buffer_ms";
    		return false;
    	}

    	if (config.enableTransmit &&
			(config.serverBind == "0.0.0.0" || config.serverBind == "::")) {
    		qWarning() << "Transmit is enabled while TCI server is bound to all interfaces."
					   << "Consider binding to 127.0.0.1 unless remote access is intentional.";
			}

    	if (config.enableTransmit && !config.txAudioKeysPtt) {
    		qWarning() << "Transmit is enabled, but tx_audio_keys_ptt is disabled."
					   << "The client must assert PTT explicitly with trx/ptt commands.";
    	}

    	if (config.radioBackend == "null" && config.enableTransmit) {
    		qWarning() << "Transmit is enabled but radio.backend=null."
					   << "PTT commands will not control real hardware.";
    	}

    	return true;
    }

	void filteredQtMessageHandler(
		QtMsgType type,
		const QMessageLogContext &context,
		const QString &message
	)
    {
    	if (type == QtWarningMsg &&
			context.category &&
			std::strcmp(context.category, "qt.core.qfuture.continuations") == 0 &&
			message.startsWith(QStringLiteral("Parent future has "))) {
    			return;
			}

    	if (type == QtInfoMsg &&
			context.category &&
			std::strcmp(context.category, "qt.multimedia.ffmpeg") == 0 &&
			message.startsWith(QStringLiteral("Using Qt multimedia with FFmpeg version"))) {
    			return;
			}

    	const QByteArray formatted =
			qFormatLogMessage(type, context, message).toLocal8Bit();

    	std::fprintf(stderr, "%s\n", formatted.constData());
    	std::fflush(stderr);

    	if (type == QtFatalMsg)
    		std::abort();
    }
}

int main(int argc, char *argv[])
{
	// Suppress harmless QT warnings
	qInstallMessageHandler(filteredQtMessageHandler);

    QCoreApplication app(argc, argv);

	if (!SignalHandler::install()) {
		qCritical() << "Failed to install signal handlers";
		return 1;
	}

	SignalHandler signalHandler;

	QCoreApplication::setApplicationName(TCI_BRIDGE_NAME);
	QCoreApplication::setApplicationVersion(TCI_BRIDGE_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription("Hamlib/audio to TCI bridge");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOption(
        QStringList() << "config" << "c",
        "YAML configuration file.",
        "file"
    );

    QCommandLineOption listAudioOption(
        QStringList() << "list-audio-devices",
        "List Qt audio input/output devices and exit."
    );

	QCommandLineOption checkConfigOption(
		QStringList() << "check-config",
		"Load and validate configuration, then exit."
	);

    QCommandLineOption bindOption(
        QStringList() << "b" << "bind",
        "Address to bind.",
        "address"
    );

    QCommandLineOption portOption(
        QStringList() << "p" << "port",
        "TCI TCP/WebSocket port to listen on.",
        "port"
    );

	QCommandLineOption serverDebugOption(
		QStringList() << "tci-debug",
		"Log TCI protocol text RX/TX traffic."
	);

    QCommandLineOption backendOption(
        QStringList() << "backend",
        "Radio backend: null or rigctld.",
        "backend"
    );

    QCommandLineOption rigHostOption(
        QStringList() << "rigctld-host",
        "rigctld host.",
        "host"
    );

    QCommandLineOption rigPortOption(
        QStringList() << "rigctld-port",
        "rigctld TCP port.",
        "port"
    );

    QCommandLineOption pollOption(
        QStringList() << "poll-ms",
        "Radio state polling interval in milliseconds.",
        "ms"
    );

    QCommandLineOption rigDebugOption(
        QStringList() << "rigctld-debug",
        "Log rigctld transactions."
    );

    QCommandLineOption audioRxOption(
        QStringList() << "audio-rx",
        "RX audio input device name. Partial match is allowed.",
        "name"
    );

    QCommandLineOption audioTxOption(
        QStringList() << "audio-tx",
        "TX audio output device name. Partial match is allowed.",
        "name"
    );

    QCommandLineOption audioDebugOption(
        QStringList() << "audio-debug",
        "Log audio frame activity."
    );

	QCommandLineOption txSinkBufferMsOption(
		QStringList() << "tx-sink-buffer-ms",
		"TX QAudioSink buffer duration in milliseconds.",
		"ms"
	);

	QCommandLineOption txPrebufferMsOption(
		QStringList() << "tx-prebuffer-ms",
		"TX audio prebuffer duration before feeding the sink, in milliseconds.",
		"ms"
	);

	QCommandLineOption txJitterBufferMsOption(
		QStringList() << "tx-jitter-buffer-ms",
		"Maximum TX jitter buffer duration in milliseconds.",
		"ms"
	);

	QCommandLineOption txDrainIntervalMsOption(
		QStringList() << "tx-drain-interval-ms",
		"TX audio drain timer interval in milliseconds.",
		"ms"
	);

    QCommandLineOption txAudioKeysPttOption(
        QStringList() << "tx-audio-keys-ptt",
        "Assert PTT while TX audio is active."
    );

	QCommandLineOption enableTransmitOption(
		QStringList() << "enable-transmit",
		"Allow tci-bridge to assert radio PTT."
	);

	QCommandLineOption maxTxMsOption(
		QStringList() << "max-tx-ms",
		"Maximum continuous TX duration in milliseconds.",
		"ms"
	);

	QCommandLineOption noUnkeyOnDisconnectOption(
		QStringList() << "no-unkey-on-disconnect",
		"Do not force PTT off when a TCI client disconnects."
	);

	QCommandLineOption quietOption(
		QStringList() << "quiet",
		"Suppress routine informational logs."
	);

	QCommandLineOption noStartupConfigOption(
		QStringList() << "no-startup-config",
		"Do not print the effective configuration at startup."
	);

	QCommandLineOption txTimingOption(
		QStringList() << "tx-timing",
		"Log TX audio frame timing diagnostics."
	);

    parser.addOption(configOption);
    parser.addOption(listAudioOption);
	parser.addOption(checkConfigOption);
    parser.addOption(bindOption);
    parser.addOption(portOption);
	parser.addOption(serverDebugOption);
    parser.addOption(backendOption);
    parser.addOption(rigHostOption);
    parser.addOption(rigPortOption);
    parser.addOption(pollOption);
    parser.addOption(rigDebugOption);
    parser.addOption(audioRxOption);
    parser.addOption(audioTxOption);
    parser.addOption(audioDebugOption);
	parser.addOption(txSinkBufferMsOption);
	parser.addOption(txPrebufferMsOption);
	parser.addOption(txJitterBufferMsOption);
	parser.addOption(txDrainIntervalMsOption);
    parser.addOption(txAudioKeysPttOption);
	parser.addOption(enableTransmitOption);
	parser.addOption(maxTxMsOption);
	parser.addOption(noUnkeyOnDisconnectOption);
	parser.addOption(quietOption);
	parser.addOption(noStartupConfigOption);
	parser.addOption(txTimingOption);

    parser.process(app);

    if (parser.isSet(listAudioOption)) {
        QtAudioBackend::listAudioDevices();
        return 0;
    }

	if (!parser.isSet(checkConfigOption)){
		qInfo().noquote()
		<< QStringLiteral("%1 %2 starting")
			   .arg(QCoreApplication::applicationName())
			   .arg(QCoreApplication::applicationVersion());
	}

    AppConfig config = AppConfig::defaults();

    if (parser.isSet(configOption)) {
        QString error;

        if (!AppConfig::loadYamlFile(parser.value(configOption), &config, &error)) {
            qCritical().noquote() << error;
            return 2;
        }
    }

    if (parser.isSet(bindOption))
        config.serverBind = parser.value(bindOption);

    if (parser.isSet(portOption)) {
        if (!parsePort(parser.value(portOption), &config.serverPort, "TCI"))
            return 2;
    }

    if (parser.isSet(backendOption))
        config.radioBackend = parser.value(backendOption).trimmed().toLower();

    if (parser.isSet(rigHostOption))
        config.rigctldHost = parser.value(rigHostOption);

    if (parser.isSet(rigPortOption)) {
        if (!parsePort(parser.value(rigPortOption), &config.rigctldPort, "rigctld"))
            return 2;
    }

	if (parser.isSet(serverDebugOption))
		config.serverDebug = true;

    if (parser.isSet(pollOption)) {
        if (!parseIntMin(parser.value(pollOption), 50, &config.pollMs, "poll-ms"))
            return 2;
    }

    if (parser.isSet(rigDebugOption))
        config.rigctldDebug = true;

    if (parser.isSet(audioRxOption))
        config.audioRxDevice = parser.value(audioRxOption);

    if (parser.isSet(audioTxOption))
        config.audioTxDevice = parser.value(audioTxOption);

    if (parser.isSet(audioDebugOption))
        config.audioDebug = true;

	if (parser.isSet(txSinkBufferMsOption)) {
		if (!parseIntMin(parser.value(txSinkBufferMsOption), 20, &config.audioTxSinkBufferMs, "tx-sink-buffer-ms"))
			return 2;
	}

	if (parser.isSet(txPrebufferMsOption)) {
		if (!parseIntMin(parser.value(txPrebufferMsOption), 0, &config.audioTxPrebufferMs, "tx-prebuffer-ms"))
			return 2;
	}

	if (parser.isSet(txJitterBufferMsOption)) {
		if (!parseIntMin(parser.value(txJitterBufferMsOption), 100, &config.audioTxJitterBufferMs, "tx-jitter-buffer-ms"))
			return 2;
	}

	if (parser.isSet(txDrainIntervalMsOption)) {
		if (!parseIntMin(parser.value(txDrainIntervalMsOption), 1, &config.audioTxDrainIntervalMs, "tx-drain-interval-ms"))
			return 2;
	}

    if (parser.isSet(txAudioKeysPttOption))
        config.txAudioKeysPtt = true;

	if (parser.isSet(enableTransmitOption))
		config.enableTransmit = true;

	if (parser.isSet(maxTxMsOption)) {
		if (!parseIntMin(parser.value(maxTxMsOption), 1000, &config.maxTxMs, "max-tx-ms"))
			return 2;
	}

	if (parser.isSet(quietOption))
		config.quiet = true;

	if (parser.isSet(noStartupConfigOption))
		config.logStartupConfig = false;

	if (parser.isSet(txTimingOption))
		config.logTxTiming = true;

	if (parser.isSet(noUnkeyOnDisconnectOption))
		config.unkeyOnDisconnect = false;

    if (config.radioBackend != "null" && config.radioBackend != "rigctld") {
        qCritical() << "Unknown backend:" << config.radioBackend;
        return 2;
    }

	if (!validateRuntimeConfig(config))
		return 2;

	if (parser.isSet(checkConfigOption)) {
		logConfig(config);
		qInfo() << "Configuration OK";
		return 0;
	}

	if (config.logStartupConfig && !config.quiet)
		logConfig(config);

	if (!config.quiet) {
		qInfo().noquote()
			<< QStringLiteral("Runtime mode: radio=%1 transmit=%2 txAudioKeysPtt=%3 bind=%4:%5")
				   .arg(config.radioBackend)
				   .arg(config.enableTransmit ? "enabled" : "disabled")
				   .arg(config.txAudioKeysPtt ? "enabled" : "disabled")
				   .arg(config.serverBind)
				   .arg(config.serverPort);
	}

    std::unique_ptr<RadioBackend> radio;

    if (config.radioBackend == "null") {
        radio = std::make_unique<NullRadioBackend>();
    } else if (config.radioBackend == "rigctld") {
        auto rig = std::make_unique<RigctldBackend>(
            config.rigctldHost,
            config.rigctldPort,
            config.pollMs
        );

        rig->setDebug(config.rigctldDebug);
        radio = std::move(rig);
    }

	TciServer server(radio.get());

	QObject::connect(&app, &QCoreApplication::aboutToQuit,
				 &server, [&server]() {
					 server.forceUnkey(QStringLiteral("application shutdown"));
				 });

	server.setDebug(config.serverDebug);
	server.setQuiet(config.quiet);
	server.setTransmitEnabled(config.enableTransmit);
	server.setTxAudioKeysPtt(config.txAudioKeysPtt);
	server.setMaxTxMs(config.maxTxMs);
	server.setUnkeyOnDisconnect(config.unkeyOnDisconnect);
	server.setTxTimingLogEnabled(config.logTxTiming);

	if (!server.listen(QHostAddress(config.serverBind), config.serverPort))
        return 1;

    qInfo() << "Initial radio state:"
            << "freq=" << radio->frequencyHz()
            << "mode=" << radio->mode()
            << "ptt=" << radio->ptt();

    QtAudioBackend audio;

    audio.setRxDeviceName(config.audioRxDevice);
    audio.setTxDeviceName(config.audioTxDevice);
    audio.setDebug(config.audioDebug);
	audio.setQuiet(config.quiet);
	audio.setTxSinkBufferMs(config.audioTxSinkBufferMs);
	audio.setTxPrebufferMs(config.audioTxPrebufferMs);
	audio.setTxJitterBufferMs(config.audioTxJitterBufferMs);
	audio.setTxDrainIntervalMs(config.audioTxDrainIntervalMs);

    QObject::connect(&audio, &QtAudioBackend::rxAudioFrame,
                     &server, [&server, &audio](const QByteArray &pcm) {
                         const QByteArray monoInt16 = PcmConvert::toMonoInt16(
                             pcm,
                             audio.rxFormat()
                         );

                         if (monoInt16.isEmpty())
                             return;

                         server.sendRxAudio(
                             monoInt16,
                             48000,
                             1
                         );
                     });

	QObject::connect(&server, &TciServer::txAudioFrame,
				 &audio, [&audio](const QByteArray &pcm) {
					 audio.writeTxAudio(pcm);
				 });

	QObject::connect(&server, &TciServer::txAudioStarted,
					 &audio, &QtAudioBackend::beginTxAudioStream);

	QObject::connect(&server, &TciServer::txAudioStopped,
					 &audio, &QtAudioBackend::endTxAudioStream);

	QObject::connect(&signalHandler, &SignalHandler::interruptReceived,
					 &server, [&server]() {
						 server.forceUnkey(QStringLiteral("SIGINT"));
					 });

	QObject::connect(&signalHandler, &SignalHandler::terminateReceived,
					 &server, [&server]() {
						 server.forceUnkey(QStringLiteral("SIGTERM"));
					 });

	QObject::connect(&app, &QCoreApplication::aboutToQuit,
					 &server, [&server]() {
						 server.forceUnkey(QStringLiteral("application shutdown"));
					 });

	QObject::connect(&app, &QCoreApplication::aboutToQuit,
					 &audio, [&audio]() {
						 audio.stopRx();
						 audio.stopTx();
					 });

    if (!audio.startRx()) {
        qWarning() << "RX audio did not start. Continuing without RX audio.";
    }

    if (!audio.startTx()) {
        qWarning() << "TX audio did not start. Continuing without TX audio.";
    }

    return app.exec();
}