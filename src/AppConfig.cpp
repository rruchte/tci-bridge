#include "AppConfig.h"

#include <yaml-cpp/yaml.h>

#include <QFileInfo>
#include <QDebug>

namespace
{
    QString qstr(const std::string &s)
    {
        return QString::fromStdString(s);
    }

    template <typename T>
    void readOptional(const YAML::Node &node, const char *key, T *target)
    {
        if (!node || !node[key])
            return;

        *target = node[key].as<T>();
    }

    void readOptionalQString(const YAML::Node &node, const char *key, QString *target)
    {
        if (!node || !node[key])
            return;

        *target = qstr(node[key].as<std::string>());
    }

    bool readOptionalPort(const YAML::Node &node, const char *key, quint16 *target, QString *error)
    {
        if (!node || !node[key])
            return true;

        const int value = node[key].as<int>();

        if (value <= 0 || value > 65535) {
            if (error)
                *error = QStringLiteral("Invalid port value for %1: %2")
                             .arg(key)
                             .arg(value);
            return false;
        }

        *target = static_cast<quint16>(value);
        return true;
    }
}

AppConfig AppConfig::defaults()
{
    return AppConfig{};
}

bool AppConfig::loadYamlFile(const QString &path, AppConfig *config, QString *error)
{
    if (!config) {
        if (error)
            *error = "Internal error: null config pointer";
        return false;
    }

    QFileInfo fileInfo(path);

    if (!fileInfo.exists() || !fileInfo.isFile()) {
        if (error)
            *error = QStringLiteral("Config file does not exist: %1").arg(path);
        return false;
    }

    try {
        const YAML::Node root = YAML::LoadFile(path.toStdString());

        const YAML::Node server = root["server"];
        readOptionalQString(server, "bind", &config->serverBind);
    	readOptional<bool>(server, "debug", &config->serverDebug);

        if (!readOptionalPort(server, "port", &config->serverPort, error))
            return false;

        const YAML::Node radio = root["radio"];
        readOptionalQString(radio, "backend", &config->radioBackend);
        readOptionalQString(radio, "rigctld_host", &config->rigctldHost);

        if (!readOptionalPort(radio, "rigctld_port", &config->rigctldPort, error))
            return false;

        readOptional<int>(radio, "poll_ms", &config->pollMs);
        readOptional<bool>(radio, "debug", &config->rigctldDebug);

        const YAML::Node audio = root["audio"];
    	readOptionalQString(audio, "mode", &config->audioMode);
        readOptionalQString(audio, "rx_device", &config->audioRxDevice);
        readOptionalQString(audio, "tx_device", &config->audioTxDevice);
        readOptional<bool>(audio, "debug", &config->audioDebug);
    	
    	readOptional<int>(audio, "tx_sink_buffer_ms", &config->audioTxSinkBufferMs);
    	readOptional<int>(audio, "tx_prebuffer_ms", &config->audioTxPrebufferMs);
    	readOptional<int>(audio, "tx_jitter_buffer_ms", &config->audioTxJitterBufferMs);
    	readOptional<int>(audio, "tx_drain_interval_ms", &config->audioTxDrainIntervalMs);

    	const YAML::Node ptt = root["ptt"];
    	readOptional<bool>(ptt, "enable_transmit", &config->enableTransmit);
    	readOptional<bool>(ptt, "tx_audio_keys_ptt", &config->txAudioKeysPtt);
    	readOptional<int>(ptt, "max_tx_ms", &config->maxTxMs);
    	readOptional<bool>(ptt, "unkey_on_disconnect", &config->unkeyOnDisconnect);

    	const YAML::Node logging = root["logging"];
    	readOptional<bool>(logging, "quiet", &config->quiet);
    	readOptional<bool>(logging, "startup_config", &config->logStartupConfig);
    	readOptional<bool>(logging, "tx_timing", &config->logTxTiming);

    	return config->normalizeAndValidate(error);
    } catch (const YAML::Exception &e) {
        if (error)
            *error = QStringLiteral("YAML error in %1: %2")
                         .arg(path)
                         .arg(e.what());
        return false;
    } catch (const std::exception &e) {
        if (error)
            *error = QStringLiteral("Error reading config %1: %2")
                         .arg(path)
                         .arg(e.what());
        return false;
    }
}

bool AppConfig::normalizeAndValidate(QString *error)
{
    serverBind = serverBind.trimmed();
    radioBackend = radioBackend.trimmed().toLower();
    rigctldHost = rigctldHost.trimmed();
    audioMode = audioMode.trimmed().toLower();
    audioRxDevice = audioRxDevice.trimmed();
    audioTxDevice = audioTxDevice.trimmed();

    if (serverBind.isEmpty()) {
        if (error) *error = QStringLiteral("server.bind must not be empty");
        return false;
    }

    if (serverPort == 0) {
        if (error) *error = QStringLiteral("server.port must be between 1 and 65535");
        return false;
    }

    if (radioBackend != "null" && radioBackend != "rigctld") {
        if (error) *error = QStringLiteral("Unsupported radio.backend: %1").arg(radioBackend);
        return false;
    }

    if (radioBackend == "rigctld") {
        if (rigctldHost.isEmpty()) {
            if (error) *error = QStringLiteral("radio.rigctld_host must not be empty when radio.backend=rigctld");
            return false;
        }

        if (rigctldPort == 0) {
            if (error) *error = QStringLiteral("radio.rigctld_port must be between 1 and 65535");
            return false;
        }
    }

    if (pollMs < 50) {
        if (error) *error = QStringLiteral("radio.poll_ms must be >= 50");
        return false;
    }

    if (audioMode.isEmpty()) {
        audioMode = "default";
    }

    if (audioMode != "default"
        && audioMode != "manual"
        && audioMode != "auto-usb-full-duplex") {
        if (error) *error = QStringLiteral("Unsupported audio.mode: %1").arg(audioMode);
        return false;
    }

    if (audioMode == "manual"
        && (audioRxDevice.isEmpty() || audioTxDevice.isEmpty())) {
        if (error) {
            *error = QStringLiteral("audio.mode=manual requires both audio.rx_device and audio.tx_device");
        }
        return false;
    }

    if (audioTxSinkBufferMs < 20) {
        if (error) *error = QStringLiteral("audio.tx_sink_buffer_ms must be >= 20");
        return false;
    }

    if (audioTxPrebufferMs < 0) {
        if (error) *error = QStringLiteral("audio.tx_prebuffer_ms must be >= 0");
        return false;
    }

    if (audioTxJitterBufferMs < 100) {
        if (error) *error = QStringLiteral("audio.tx_jitter_buffer_ms must be >= 100");
        return false;
    }

    if (audioTxDrainIntervalMs < 1) {
        if (error) *error = QStringLiteral("audio.tx_drain_interval_ms must be >= 1");
        return false;
    }

    if (audioTxPrebufferMs >= audioTxJitterBufferMs) {
        if (error) {
            *error = QStringLiteral("audio.tx_prebuffer_ms must be less than audio.tx_jitter_buffer_ms");
        }
        return false;
    }

    if (maxTxMs < 1000) {
        if (error) *error = QStringLiteral("ptt.max_tx_ms must be >= 1000");
        return false;
    }

    return true;
}