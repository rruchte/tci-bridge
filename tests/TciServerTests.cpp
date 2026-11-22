#include "TciServer.h"
#include "RigctldBackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <functional>
#include <vector>

namespace
{
class TestRadio : public RadioBackend
{
public:
    qint64 frequencyHz() const override { return frequency; }
    void setFrequencyHz(qint64 hz) override { frequencyRequest = hz; }
    qint64 txFrequencyHz() const override { return frequency; }
    void setTxFrequencyHz(qint64) override {}
    bool splitEnabled() const override { return false; }
    void setSplitEnabled(bool) override {}
    QString mode() const override { return QStringLiteral("usb"); }
    void setMode(const QString &) override {}
    bool ptt() const override { return keyed; }
    bool online() const override { return true; }
    void setPollingSuspended(bool) override {}
    bool supportsCompleteTxMeter() const override { return true; }
    void setTxMeterInterval(int ms) override { txMeterInterval = ms; }
    void setPtt(bool enabled) override { requests.push_back(enabled); }

    void confirmPtt(bool enabled)
    {
        if (keyed != enabled) {
            keyed = enabled;
            emit pttChanged(enabled);
        }
        emit pttCommandSucceeded(enabled);
    }

    qint64 frequency = 7100000;
    qint64 frequencyRequest = 0;
    bool keyed = false;
    int txMeterInterval = 0;
    std::vector<bool> requests;
};

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 2500)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

struct Client {
    QWebSocket socket;
    QStringList messages;
    QList<TciStream::StreamFrame> frames;

    explicit Client(quint16 port)
    {
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket,
                         [this](const QString &text) { messages.append(text); });
        QObject::connect(&socket, &QWebSocket::binaryMessageReceived, &socket,
                         [this](const QByteArray &frame) { frames.append(TciStream::parseFrame(frame)); });
        socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(port)));
    }

    void send(const QString &message) { socket.sendTextMessage(message); }
    bool saw(const QString &message) const { return messages.contains(message); }
};

bool exerciseCatAcknowledgements()
{
    QTcpServer rigctld;
    qint64 frequency = 7100000;
    bool rejected = false;
    int keyWrites = 0;
    int unkeyWrites = 0;
    bool rejectUnkey = false;
    bool keyRequested = false;
    bool split = false;
    qint64 splitHz = 7250000;
    int clarifierOffset = 150;
    bool ritEnabled = false;
    bool xitEnabled = false;
    bool nbEnabled = false;
    bool nrSupported = false;
    int nrMode = 2;
    int nrReads = 0;
    double rfPower = 0.25;
    int agc = 4;
    QByteArray receiveMode = "USB";
    int filterLow = 100;
    int filterHigh = 2700;
    int slopeReads = 0;
    int meterReads = 0;
    int meterDb = -18;
    int txPowerReads = 0;
    bool txPowerSupported = true;
    bool meterSupported = true;
    QStringList agcWrites;
    int powerReads = 0;
    QStringList powerWrites;
    QObject::connect(&rigctld, &QTcpServer::newConnection, &rigctld, [&] {
        auto *socket = rigctld.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            while (socket->canReadLine()) {
                const QByteArray command = socket->readLine().trimmed();
                if (command == "f") {
                    socket->write(QByteArray::number(frequency) + "\n");
                } else if (command == "m") {
                    socket->write(receiveMode + "\n2400\n");
                } else if (command == "t") {
                    socket->write(keyRequested ? "1\n" : "0\n");
                } else if (command == "s") {
                    socket->write(split ? "1\n" : "0\n");
                    QTimer::singleShot(10, socket, [socket] { socket->write("VFOB\n"); });
                } else if (command == "i") {
                    if (split)
                        socket->write(QByteArray::number(splitHz) + "\n");
                    else
                        socket->write("RPRT -1\n");
                } else if (command == "u NB") {
                    socket->write(nbEnabled ? "1\n" : "0\n");
                } else if (command == "u NR") {
                    ++nrReads;
                    if (nrSupported)
                        socket->write(QByteArray::number(nrMode) + "\n");
                    else
                        socket->write("RPRT -11\n");
                } else if (command == "U NB 1" || command == "U NB 0") {
                    nbEnabled = command.endsWith('1');
                    socket->write("RPRT 0\n");
                } else if (command == "U NR 0" && nrSupported) {
                    nrMode = 0;
                    socket->write("RPRT 0\n");
                } else if (command == "u RIT" || command == "u XIT") {
                    const bool enabled = command == "u RIT" ? ritEnabled : xitEnabled;
                    socket->write(enabled ? "1\n" : "0\n");
                } else if (command == "j" || command == "z") {
                    socket->write(QByteArray::number(clarifierOffset) + "\n");
                } else if (command.startsWith("U RIT ") || command.startsWith("U XIT ")) {
                    bool &enabled = command.startsWith("U RIT ") ? ritEnabled : xitEnabled;
                    enabled = command.endsWith('1');
                    socket->write("RPRT 0\n");
                } else if (command.startsWith("J ") || command.startsWith("Z ")) {
                    clarifierOffset = qBound(-9999, command.mid(2).toInt(), 9999);
                    socket->write("RPRT 0\n");
                } else if (command == "l SLOPE_LOW" || command == "l SLOPE_HIGH") {
                    ++slopeReads;
                    const int value = command == "l SLOPE_LOW" ? filterLow : filterHigh;
                    socket->write(QByteArray::number(value) + "\n");
                } else if (command.startsWith("L SLOPE_LOW ")) {
                    filterLow = command.mid(12).toInt();
                    socket->write("RPRT 0\n");
                } else if (command.startsWith("L SLOPE_HIGH ")) {
                    const int requested = command.mid(13).toInt();
                    if (requested == 2900) {
                        socket->write("RPRT -9\n");
                    } else {
                        filterHigh = requested;
                        socket->write("RPRT 0\n");
                    }
                } else if (command == "l RFPOWER_METER_WATTS") {
                    ++txPowerReads;
                    socket->write(txPowerSupported ? "25.5\n" : "RPRT -11\n");
                } else if (command == "l SWR") {
                    socket->write("1.4\n");
                } else if (command == "l STRENGTH") {
                    ++meterReads;
                    if (meterSupported)
                        socket->write(QByteArray::number(meterDb) + "\n");
                    else
                        socket->write("RPRT -11\n");
                } else if (command == "l AGC") {
                    socket->write(QByteArray::number(agc) + "\n");
                } else if (command.startsWith("L AGC ")) {
                    agcWrites.append(QString::fromLatin1(command));
                    if (command == "L AGC 6") {
                        socket->write("RPRT -1\n");
                    } else {
                        agc = command.mid(6).toInt();
                        socket->write("RPRT 0\n");
                    }
                } else if (command == "l RFPOWER") {
                    const QByteArray reply = QByteArray::number(rfPower) + "\n";
                    if (++powerReads == 1)
                        socket->write("RPRT -11\n");
                    else if (powerReads == 2)
                        QTimer::singleShot(325, socket, [socket, reply] { socket->write(reply); });
                    else
                        socket->write(reply);
                } else if (command.startsWith("L RFPOWER ")) {
                    powerWrites.append(QString::fromLatin1(command));
                    if (command == "L RFPOWER 0.40") {
                        rfPower = 0.375;
                        socket->write("RPRT 0\n");
                    } else {
                        socket->write("RPRT -9\n");
                    }
                } else if (command == "T 1") {
                    ++keyWrites;
                    socket->write("RPRT 0\n");
                } else if (command == "T 0") {
                    ++unkeyWrites;
                    if (rejectUnkey) {
                        socket->write("RPRT -9\n");
                    } else {
                        keyRequested = false;
                        socket->write("RPRT 0\n");
                    }
                } else if (command.startsWith("F ")) {
                    if (command == "F 7200000") {
                        frequency = 7200000;
                        // Exercise a fragmented Hamlib status line.
                        socket->write("RPRT ");
                        QTimer::singleShot(20, socket, [socket] { socket->write("0\n"); });
                    } else {
                        rejected = true;
                        socket->write("RPRT -1\n");
                    }
                } else if (command == "dump_state" || command == "\\dump_state") {
                    // The mock does not implement capability discovery. Return a
                    // normal Hamlib failure instead of closing the CAT socket.
                    // Explicit dropCat() calls below exercise reconnection.
                    socket->write("RPRT -11\n");
                } else {
                    socket->write("RPRT -1\n");
                }
            }
        });
    });
    if (!rigctld.listen(QHostAddress::LocalHost, 0))
        return false;
    RigctldBackend radio(QStringLiteral("127.0.0.1"), rigctld.serverPort(), 250);
    if (!waitUntil([&] { return radio.online(); }))
        return false;

    radio.setFrequencyHz(7300000);
    if (radio.frequencyHz() != 7100000 || !waitUntil([&] { return rejected; }))
        return false;

    radio.setFrequencyHz(7200000);
    if (radio.frequencyHz() != 7100000 ||
        !waitUntil([&] { return radio.frequencyHz() == 7200000; }))
        return false;

    TciServer bridge(&radio);
    bridge.setAllowPartialTxSensors(true);
    bridge.setTransmitEnabled(true);
    if (!bridge.listen(QHostAddress::LocalHost, 0))
        return false;
    Client client(bridge.serverPort());
    if (!waitUntil([&] { return client.saw("tx_frequency:7200000;"); }))
        return false;
    split = true;
    if (!waitUntil([&] { return radio.splitEnabled() && client.saw("tx_frequency:7250000;"); }))
        return false;
    splitHz = 7260000;
    if (!waitUntil([&] { return client.saw("tx_frequency:7260000;"); }))
        return false;
    client.messages.clear();
    split = false;
    if (!waitUntil([&] { return !radio.splitEnabled() && client.saw("tx_frequency:7200000;"); }))
        return false;

    client.send("rit_enable:0;");
    client.send("xit_offset:0;");
    if (!waitUntil([&] { return client.saw("rit_enable:0,false;") &&
                               client.saw("xit_offset:0,150;"); }))
        return false;
    Client observer(bridge.serverPort());
    if (!waitUntil([&] { return observer.saw("xit_offset:0,150;"); }))
        return false;
    client.send("xit_enable:0,true;");
    client.send("rit_offset:0,-10000;");
    if (!waitUntil([&] { return observer.saw("xit_enable:0,true;") &&
                               observer.saw("rit_offset:0,-9999;") &&
                               observer.saw("xit_offset:0,-9999;") &&
                               observer.saw("tx_frequency:7190001;"); }) || ritEnabled)
        return false;

    client.messages.clear();
    client.send("xit_offset:0,0;");
    if (!waitUntil([&] { return client.saw("xit_offset:0,0;") &&
                               client.saw("tx_frequency:7200000;"); }) || !xitEnabled)
        return false;
    observer.messages.clear();
    ritEnabled = true;
    if (!waitUntil([&] { return observer.saw("rit_enable:0,true;"); }, 5500))
        return false;

    observer.send("drive:0;");
    if (!waitUntil([&] { return observer.saw("drive:0,25;"); }))
        return false;
    client.send("drive:1,40;");
    client.send("drive:0,101;");
    client.send("drive:0,40;");
    if (!waitUntil([&] { return observer.saw("drive:0,38;"); }) ||
        powerWrites != QStringList{QStringLiteral("L RFPOWER 0.40")})
        return false;

    client.send("drive:0,60;");
    if (!waitUntil([&] { return powerWrites.size() == 2; }))
        return false;
    client.messages.clear();
    client.send("drive:0;");
    if (!waitUntil([&] { return client.saw("drive:0,38;"); }) || client.saw("drive:0,60;"))
        return false;
    rfPower = 0.55;
    if (!waitUntil([&] { return observer.saw("drive:0,55;"); }, 5500))
        return false;

    client.send("agc_mode:0,invalid;");
    client.send("agc_mode:1,fast;");
    client.send("agc_mode:0,NORMAL;");
    if (!waitUntil([&] { return observer.saw("agc_mode:0,normal;"); }) ||
        agcWrites != QStringList{"L AGC 6", "L AGC 5"})
        return false;
    observer.messages.clear();
    observer.send("agc_mode:0,normal;");
    if (!waitUntil([&] { return observer.saw("agc_mode:0,normal;"); }) ||
        agcWrites.size() != 2)
        return false;
    agc = 1; // Native SUPERFAST maps to TCI fast.
    if (!waitUntil([&] { return observer.saw("agc_mode:0,fast;"); }, 5500))
        return false;
    client.send("agc_mode:0,off;");
    if (!waitUntil([&] { return observer.saw("agc_mode:0,off;"); }) ||
        agcWrites.size() != 3 || agc != 0)
        return false;

    client.messages.clear();
    client.send("rx_filter_band:0;");
    client.send("agc_mode:0;");
    if (!waitUntil([&] { return client.saw("agc_mode:0,off;"); }) || slopeReads != 0)
        return false;

    // A width-only radio still delivers PCM to TCI subscribers.
    client.send("audio_start:0;");
    client.messages.clear();
    client.send("start;");
    if (!waitUntil([&] { return client.saw("vfo:0,0,7200000;"); }))
        return false;
    bridge.sendRxAudio(QByteArray(4096, '\0'), 48000, 1);
    if (!waitUntil([&] { return !client.frames.isEmpty(); }) ||
        !client.frames.front().valid ||
        client.frames.front().streamType != TciStream::RX_AUDIO_STREAM ||
        !observer.frames.isEmpty())
        return false;
    client.send("audio_stop:0;");

    radio.setFilterEdgesHzEnabled(true);
    client.send("rx_filter_band:0;");
    if (!waitUntil([&] { return observer.saw("rx_filter_band:0,100,2700;"); }))
        return false;
    client.send("rx_filter_band:0,200,2900;");
    if (!waitUntil([&] { return observer.saw("rx_filter_band:0,200,2700;"); }))
        return false;

    receiveMode = "LSB";
    client.send("rx_filter_band:0,-2600,-300;");
    if (!waitUntil([&] { return observer.saw("rx_filter_band:0,-2600,-300;"); }) ||
        filterLow != 300 || filterHigh != 2600)
        return false;
    filterHigh = 2800;
    if (!waitUntil([&] { return observer.saw("rx_filter_band:0,-2800,-300;"); }, 5500))
        return false;

    client.send("rx_nb_enable:0,true;");
    if (!waitUntil([&] { return observer.saw("rx_nb_enable:0,true;"); }) || !nbEnabled)
        return false;
    const int previousNrReads = nrReads;
    client.send("rx_nr_enable:0;");
    if (!waitUntil([&] { return nrReads > previousNrReads; }) ||
        client.saw("rx_nr_enable:0,false;") || client.saw("rx_nr_enable:0,true;"))
        return false;
    nrSupported = true;
    client.send("rx_nr_enable:0;");
    if (!waitUntil([&] { return observer.saw("rx_nr_enable:0,true;"); }))
        return false;
    client.send("rx_nr_enable:0,false;");
    if (!waitUntil([&] { return observer.saw("rx_nr_enable:0,false;"); }) || nrMode != 0)
        return false;
    nbEnabled = false;
    observer.messages.clear();
    if (!waitUntil([&] { return observer.saw("rx_nb_enable:0,false;"); }, 5500))
        return false;

    if (meterReads != 0)
        return false;
    client.send("rx_sensors_enable:true,30;");
    if (!waitUntil([&] { return client.saw("rx_sensors:0,-91;") &&
                               client.saw("rx_channel_sensors:0,0,-91;"); }) ||
        observer.saw("rx_sensors:0,-91;"))
        return false;
    observer.send("rx_sensors_enable:true,1000;");
    if (!waitUntil([&] { return observer.saw("rx_sensors:0,-91;"); }))
        return false;
    client.send("rx_sensors_enable:false;");
    client.send("agc_mode:0;");
    client.messages.clear();
    if (!waitUntil([&] { return client.saw("agc_mode:0,off;"); }))
        return false;
    frequency = 50000000;
    if (!waitUntil([&] { return observer.saw("rx_sensors:0,-111;"); }) ||
        client.saw("rx_sensors:0,-111;"))
        return false;

    observer.messages.clear();
    meterSupported = false;
    const int previousReads = meterReads;
    if (!waitUntil([&] { return meterReads > previousReads; }) ||
        observer.saw("rx_sensors:0,-111;"))
        return false;
    const quint16 catPort = rigctld.serverPort();
    const auto dropCat = [&] {
        rigctld.close();
        for (auto *socket : rigctld.findChildren<QTcpSocket *>())
            socket->abort();
    };
    observer.messages.clear();
    dropCat();
    if (!waitUntil([&] { return !radio.online() && observer.saw("tx_enable:0,false;"); }, 6500))
        return false;
    meterSupported = true;
    if (!rigctld.listen(QHostAddress::LocalHost, catPort)) {
        qCritical() << "CAT test cannot resume rigctld mock listener";
        return false;
    }
    if (!waitUntil([&] { return radio.online() && observer.saw("tx_enable:0,true;") &&
                               observer.saw("rx_sensors:0,-111;"); }, 15000)) {
        qCritical() << "CAT reconnect state missing"
                    << "online=" << radio.online()
                    << "txEnabled=" << observer.saw("tx_enable:0,true;")
                    << "rxMeter=" << observer.saw("rx_sensors:0,-111;")
                    << "observerRecentMessages=" << observer.messages.mid(qMax(0, observer.messages.size() - 20));
        return false;
    }

    client.send("tx_sensors_enable:true,30;");
    if (txPowerReads != 0)
        return false;

    bool keyFailed = false;
    QObject::connect(&radio, &RadioBackend::pttCommandFailed, &rigctld,
                     [&](bool requested) { keyFailed = requested; });
    radio.setPtt(true);
    if (!waitUntil([&] { return keyFailed; }) || keyWrites != 1 || radio.ptt())
        return false;
    keyRequested = true;
    radio.setPtt(true);
    if (!waitUntil([&] { return radio.ptt() && client.saw("tx_sensors:0,nan,25.5,nan,1.4;"); }) ||
        observer.saw("tx_sensors:0,nan,25.5,nan,1.4;"))
        return false;
    txPowerSupported = false;
    if (!waitUntil([&] { return client.saw("tx_sensors:0,nan,nan,nan,1.4;"); }))
        return false;
    observer.messages.clear();
    dropCat();
    if (!waitUntil([&] { return !radio.online() && observer.saw("tx_enable:0,false;"); }, 6500))
        return false;
    rejectUnkey = true;
    const int unkeysBeforeReconnect = unkeyWrites;
    client.messages.clear();
    if (!rigctld.listen(QHostAddress::LocalHost, catPort) ||
        !waitUntil([&] { return radio.online() && unkeyWrites > unkeysBeforeReconnect; }, 6500))
        return false;

    const int keysBeforeRetry = keyWrites;
    observer.messages.clear();
    observer.send("trx:0,true;");
    observer.send("tx_enable:0;");
    if (!waitUntil([&] { return observer.saw("tx_enable:0,false;"); }) ||
        keyWrites != keysBeforeRetry || client.saw("tx_sensors:0,nan,nan,nan,1.4;"))
        return false;

    rejectUnkey = false;
    meterDb = -14;
    return waitUntil([&] { return !radio.ptt() && observer.saw("trx:0,false;") &&
                                 observer.saw("tx_enable:0,true;") &&
                                 observer.saw("rx_sensors:0,-107;"); }, 6500);
}

bool exerciseCompleteTxMeter()
{
    TestRadio radio;
    TciServer server(&radio);
    if (!server.listen(QHostAddress::LocalHost, 0))
        return false;
    Client client(server.serverPort());
    if (!waitUntil([&] { return client.saw("ready;"); }))
        return false;
    client.send("tx_sensors_enable:true,30;");
    if (!waitUntil([&] { return radio.txMeterInterval == 30; }))
        return false;
    radio.confirmPtt(true);
    emit radio.txMeterReading(TxMeterReading{{}, 40.2, {}, 1.3});
    emit radio.txMeterReading(TxMeterReading{-27.2, 40.2, 55.7, 1.3});
    if (!waitUntil([&] { return client.saw("tx_sensors:0,-27.2,40.2,55.7,1.3;"); }) ||
        client.saw("tx_sensors:0,nan,40.2,nan,1.3;"))
        return false;
    radio.confirmPtt(false);
    client.send("tx_sensors_enable:false;");
    return waitUntil([&] { return radio.txMeterInterval == 0; });
}

bool exercise()
{
    TestRadio radio;
    TciServer server(&radio);
    server.setTransmitEnabled(true);
    server.setMaxTxMs(1000);
    if (!server.listen(QHostAddress::LocalHost, 0))
        return false;

    int prebuffer = -1;
    int audioPackets = 0;
    QObject::connect(&server, &TciServer::txAudioStarted, &server,
                     [&](int milliseconds) { prebuffer = milliseconds; });
    QObject::connect(&server, &TciServer::txAudioFrame, &server,
                     [&](const QByteArray &) { ++audioPackets; });
    Client owner(server.serverPort());
    Client observer(server.serverPort());

    if (!waitUntil([&] { return owner.saw("ready;") && observer.saw("ready;"); }))
        return false;

    owner.send("vfo:0,0,7200000;");
    if (!waitUntil([&] { return radio.frequencyRequest == 7200000; }) ||
        owner.saw("vfo:0,0,7200000;") || observer.saw("vfo:0,0,7200000;"))
        return false;
    radio.frequency = radio.frequencyRequest;
    emit radio.frequencyChanged(radio.frequency);
    if (!waitUntil([&] { return owner.saw("vfo:0,0,7200000;") &&
                               observer.saw("tx_frequency:7200000;"); }))
        return false;

    owner.send("audio_samplerate:12000;");
    owner.send("audio_stream_samples;");
    owner.send("tx_stream_audio_buffering:150;");
    observer.send("audio_stream_samples;");
    if (!waitUntil([&] { return owner.saw("audio_stream_samples:512;") &&
                               owner.saw("tx_stream_audio_buffering:150;") &&
                               observer.saw("audio_stream_samples:2048;"); }))
        return false;

    owner.send("trx:1,true,tci;");
    owner.send("trx:0,true,mic2;");
    owner.send("trx:0,true,tci;");
    if (!waitUntil([&] { return !radio.requests.empty(); }) ||
        radio.requests != std::vector<bool>{true} || prebuffer != -1 ||
        !owner.frames.isEmpty() || owner.saw("trx:0,true;"))
        return false;

    radio.confirmPtt(true);
    if (!waitUntil([&] { return owner.saw("trx:0,true;"); }) ||
        prebuffer != 150 || !owner.frames.isEmpty())
        return false;

    server.txAudioOutputReady();
    if (!waitUntil([&] { return !owner.frames.isEmpty(); }))
        return false;
    const auto &chrono = owner.frames.first();
    if (!chrono.valid || chrono.streamType != TciStream::TX_CHRONO ||
        chrono.sampleRate != 12000 || chrono.sampleType != TciStream::FLOAT32 ||
        chrono.channels != 2 || chrono.sampleCount != 512 || chrono.count != 0 ||
        !observer.frames.isEmpty())
        return false;

    QByteArray audio = TciStream::makeRxAudioFrame(
        QByteArray(512 * 4, char(0)), 0, 12000, TciStream::FLOAT32, 2);
    audio[24] = TciStream::TX_AUDIO_STREAM;
    auto sentThrough = owner.frames.size();
    if (!waitUntil([&] {
            if (owner.frames.size() > sentThrough) {
                sentThrough = owner.frames.size();
                owner.socket.sendBinaryMessage(audio);
            }
            return radio.requests.size() == 2;
        }, 1500) || radio.requests.back() || audioPackets < 2)
        return false;

    owner.messages.clear();
    observer.send("trx:0,true,tci;");
    radio.pttCommandFailed(false);
    if (!waitUntil([&] { return radio.requests.size() == 3; }) || radio.requests.back() ||
        owner.saw("trx:0,false;"))
        return false;
    radio.confirmPtt(false);
    if (!waitUntil([&] { return owner.saw("trx:0,false;"); }))
        return false;

    owner.send("trx:0,true,tci;");
    if (!waitUntil([&] { return radio.requests.size() == 4; }) || !radio.requests.back())
        return false;
    radio.pttCommandFailed(true);
    return waitUntil([&] { return radio.requests.size() == 5; }) && !radio.requests.back();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (exercise() && exerciseCompleteTxMeter() && exerciseCatAcknowledgements())
        return 0;
    qCritical("TCI wire sequencing regression");
    return 1;
}
