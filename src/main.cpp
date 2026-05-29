#include <QCoreApplication>
#include <QCommandLineParser>
#include <QHostAddress>
#include <QDebug>
#include <QStringList>

#include <memory>

#include "AppConfig.h"
#include "NullRadioBackend.h"
#include "RigctldBackend.h"
#include "TciServer.h"
#include "QtAudioBackend.h"
#include "PcmConvert.h"

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
        qInfo().noquote() << QStringLiteral("  radio.backend=%1").arg(config.radioBackend);
        qInfo().noquote() << QStringLiteral("  radio.rigctld_host=%1").arg(config.rigctldHost);
        qInfo().noquote() << QStringLiteral("  radio.rigctld_port=%1").arg(config.rigctldPort);
        qInfo().noquote() << QStringLiteral("  radio.poll_ms=%1").arg(config.pollMs);
        qInfo().noquote() << QStringLiteral("  radio.debug=%1").arg(config.rigctldDebug ? "true" : "false");
        qInfo().noquote() << QStringLiteral("  audio.rx_device=\"%1\"").arg(config.audioRxDevice);
        qInfo().noquote() << QStringLiteral("  audio.tx_device=\"%1\"").arg(config.audioTxDevice);
        qInfo().noquote() << QStringLiteral("  audio.debug=%1").arg(config.audioDebug ? "true" : "false");
        qInfo().noquote() << QStringLiteral("  ptt.tx_audio_keys_ptt=%1").arg(config.txAudioKeysPtt ? "true" : "false");
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCoreApplication::setApplicationName("tci-bridge");
    QCoreApplication::setApplicationVersion("0.4.0");

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

    QCommandLineOption txAudioKeysPttOption(
        QStringList() << "tx-audio-keys-ptt",
        "Assert PTT while TX audio is active."
    );

    parser.addOption(configOption);
    parser.addOption(listAudioOption);
    parser.addOption(bindOption);
    parser.addOption(portOption);
    parser.addOption(backendOption);
    parser.addOption(rigHostOption);
    parser.addOption(rigPortOption);
    parser.addOption(pollOption);
    parser.addOption(rigDebugOption);
    parser.addOption(audioRxOption);
    parser.addOption(audioTxOption);
    parser.addOption(audioDebugOption);
    parser.addOption(txAudioKeysPttOption);

    parser.process(app);

    if (parser.isSet(listAudioOption)) {
        QtAudioBackend::listAudioDevices();
        return 0;
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

    if (parser.isSet(txAudioKeysPttOption))
        config.txAudioKeysPtt = true;

    if (config.radioBackend != "null" && config.radioBackend != "rigctld") {
        qCritical() << "Unknown backend:" << config.radioBackend;
        return 2;
    }

    logConfig(config);

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
    server.setTxAudioKeysPtt(config.txAudioKeysPtt);

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

    if (!audio.startRx()) {
        qWarning() << "RX audio did not start. Continuing without RX audio.";
    }

    if (!audio.startTx()) {
        qWarning() << "TX audio did not start. Continuing without TX audio.";
    }

    return app.exec();
}