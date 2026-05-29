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
        readOptionalQString(audio, "rx_device", &config->audioRxDevice);
        readOptionalQString(audio, "tx_device", &config->audioTxDevice);
        readOptional<bool>(audio, "debug", &config->audioDebug);

        const YAML::Node ptt = root["ptt"];
        readOptional<bool>(ptt, "tx_audio_keys_ptt", &config->txAudioKeysPtt);

        if (config->pollMs < 50) {
            if (error)
                *error = QStringLiteral("radio.poll_ms must be >= 50");
            return false;
        }

        config->radioBackend = config->radioBackend.trimmed().toLower();

        if (config->radioBackend != "null" && config->radioBackend != "rigctld") {
            if (error)
                *error = QStringLiteral("Unsupported radio.backend: %1")
                             .arg(config->radioBackend);
            return false;
        }

        return true;
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