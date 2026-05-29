#include "TciClient.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QDebug>

#include <cmath>

namespace
{
    QByteArray makeToneFrame(
        int startSample,
        int sampleCount,
        int sampleRate,
        double toneHz
    )
    {
        QByteArray out;
        out.reserve(sampleCount * 2);

        for (int i = 0; i < sampleCount; ++i) {
            const int n = startSample + i;
            const double phase = 2.0 * M_PI * toneHz * n / sampleRate;
            const qint16 value = static_cast<qint16>(12000.0 * std::sin(phase));

            out.append(static_cast<char>(value & 0xff));
            out.append(static_cast<char>((value >> 8) & 0xff));
        }

        return out;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCoreApplication::setApplicationName("tci-client-qt");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Standalone Qt TCI client harness");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption urlOption(
        QStringList() << "url",
        "TCI WebSocket URL.",
        "url",
        "ws://127.0.0.1:40001"
    );

    QCommandLineOption freqOption(
        QStringList() << "freq",
        "Frequency to set.",
        "hz",
        "7078000"
    );

    QCommandLineOption modeOption(
        QStringList() << "mode",
        "Mode to set.",
        "mode",
        "usb"
    );

    QCommandLineOption rxFramesOption(
        QStringList() << "rx-frames",
        "Number of RX audio frames to receive before exiting.",
        "count",
        "50"
    );

    QCommandLineOption txToneOption(
        QStringList() << "tx-tone",
        "Send a short TX audio tone."
    );

    parser.addOption(urlOption);
    parser.addOption(freqOption);
    parser.addOption(modeOption);
    parser.addOption(rxFramesOption);
    parser.addOption(txToneOption);

    parser.process(app);

    bool freqOk = false;
    const qint64 freqHz = parser.value(freqOption).toLongLong(&freqOk);

    if (!freqOk || freqHz <= 0) {
        qCritical() << "Invalid frequency:" << parser.value(freqOption);
        return 2;
    }

    bool rxFramesOk = false;
    const int rxFrameLimit = parser.value(rxFramesOption).toInt(&rxFramesOk);

    if (!rxFramesOk || rxFrameLimit < 0) {
        qCritical() << "Invalid rx-frames:" << parser.value(rxFramesOption);
        return 2;
    }

    const bool sendTxTone = parser.isSet(txToneOption);

    TciClient client;

    int rxFrames = 0;
    int txSampleCursor = 0;

    QTimer txTimer;
    txTimer.setInterval(512 * 1000 / 48000);

    QObject::connect(&client, &TciClient::connected,
                     &client, [&client]() {
                         qInfo() << "Harness connected";
                     });

    QObject::connect(&client, &TciClient::ready,
                     &client, [&client, freqHz, &parser, &txTimer, sendTxTone]() {
                         qInfo() << "Harness saw ready; configuring session";

                         client.queryFrequency();
                         client.setFrequency(freqHz);

                         client.queryMode();
                         client.setMode(parser.value("mode"));

                         client.queryPtt();

                         client.configureAudio(48000, 1, 512);
                         client.startRxAudio();

                         if (sendTxTone) {
                             client.startTxAudio();
                             txTimer.start();
                         }
                     });

    QObject::connect(&client, &TciClient::frequencyChanged,
                     &client, [](qint64 hz) {
                         qInfo() << "frequencyChanged:" << hz;
                     });

    QObject::connect(&client, &TciClient::modeChanged,
                     &client, [](const QString &mode) {
                         qInfo() << "modeChanged:" << mode;
                     });

    QObject::connect(&client, &TciClient::pttChanged,
                     &client, [](bool enabled) {
                         qInfo() << "pttChanged:" << enabled;
                     });

    QObject::connect(&client, &TciClient::rxAudioFrame,
                     &client, [&client, &rxFrames, rxFrameLimit, sendTxTone, &txTimer](const QByteArray &pcm, int sampleRate) {
                         ++rxFrames;

                         if (rxFrames % 10 == 0) {
                             qInfo() << "RX audio frames:"
                                     << rxFrames
                                     << "lastBytes="
                                     << pcm.size()
                                     << "sampleRate="
                                     << sampleRate;
                         }

                         if (rxFrameLimit > 0 && rxFrames >= rxFrameLimit) {
                             qInfo() << "RX frame limit reached";

                             client.stopRxAudio();

                             if (sendTxTone) {
                                 txTimer.stop();
                                 client.stopTxAudio();
                             }

                             QTimer::singleShot(250, &client, [&client]() {
                                 client.disconnectFromServer();
                             });
                         }
                     });

    QObject::connect(&txTimer, &QTimer::timeout,
                     &client, [&client, &txSampleCursor]() {
                         constexpr int samplesPerFrame = 512;
                         constexpr int sampleRate = 48000;

                         const QByteArray pcm = makeToneFrame(
                             txSampleCursor,
                             samplesPerFrame,
                             sampleRate,
                             1000.0
                         );

                         txSampleCursor += samplesPerFrame;
                         client.sendTxAudioMonoInt16(pcm);
                     });

    QObject::connect(&client, &TciClient::disconnected,
                     &app, &QCoreApplication::quit);

    QObject::connect(&client, &TciClient::error,
                     &app, [](const QString &message) {
                         qWarning() << "TCI client error:" << message;
                     });

    client.connectToServer(QUrl(parser.value(urlOption)));

    return app.exec();
}