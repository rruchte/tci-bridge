#include "AudioDeviceResolver.h"

#include <QAudioFormat>
#include <QMediaDevices>

#include <algorithm>

namespace {

struct AudioDeviceInfo {
    QAudioDevice device;
    QString description;
    QString id;
    QAudioFormat preferredFormat;
    bool isInput = false;
};

struct AudioDevicePair {
    QAudioDevice input;
    QAudioDevice output;
    QString inputId;
    QString outputId;
    int score = 0;
    QStringList reasons;
};

QString normalizedAudioString(QString value)
{
    value = value.toLower();
    value.replace('_', ' ');
    value.replace('-', ' ');
    value.replace('.', ' ');
    value.replace(':', ' ');
    value.replace('/', ' ');
    value = value.simplified();
    return value;
}

bool containsAny(const QString &value, const QStringList &needles)
{
    const QString normalized = normalizedAudioString(value);

    for (const QString &needle : needles) {
        if (normalized.contains(normalizedAudioString(needle))) {
            return true;
        }
    }

    return false;
}

QString combinedIdentity(const AudioDeviceInfo &info)
{
    return info.description + QStringLiteral(" ") + info.id;
}

bool isObviouslyNonRadioAudio(const AudioDeviceInfo &info)
{
    const QString combined = combinedIdentity(info);

    if (containsAny(combined, {
            QStringLiteral("hdmi"),
            QStringLiteral("vc4 hdmi"),
            QStringLiteral("bcm2835 headphones"),
            QStringLiteral("built in audio"),
            QStringLiteral("built-in audio"),
            QStringLiteral("platform fe00b840"),
            QStringLiteral("mailbox stereo fallback"),
        })) {
        return true;
    }

    if (info.isInput && containsAny(combined, {
            QStringLiteral(".monitor"),
            QStringLiteral(" monitor"),
        })) {
        return true;
    }

    return false;
}

bool looksLikeUsbAudio(const AudioDeviceInfo &info)
{
    const QString combined = combinedIdentity(info);

    return containsAny(combined, {
        QStringLiteral("usb"),
        QStringLiteral("pcm290"),
        QStringLiteral("burrbrown"),
        QStringLiteral("burr brown"),
        QStringLiteral("texas instruments"),
        QStringLiteral("audio codec"),
        QStringLiteral("codec analog stereo"),
    });
}

bool looksLikeKnownUsbCodec(const AudioDeviceInfo &input, const AudioDeviceInfo &output)
{
    const QString combined =
        input.description + QStringLiteral(" ") +
        input.id + QStringLiteral(" ") +
        output.description + QStringLiteral(" ") +
        output.id;

    return containsAny(combined, {
        QStringLiteral("pcm2903"),
        QStringLiteral("pcm290"),
        QStringLiteral("usb audio codec"),
        QStringLiteral("audio codec"),
        QStringLiteral("burrbrown"),
        QStringLiteral("burr brown"),
        QStringLiteral("texas instruments"),
    });
}

bool sameDisplayDevice(const AudioDeviceInfo &input, const AudioDeviceInfo &output)
{
    return normalizedAudioString(input.description)
        == normalizedAudioString(output.description);
}

QString audioParentKey(QString value)
{
    QString s = normalizedAudioString(value);

    s.replace(QStringLiteral("alsa input"), QString());
    s.replace(QStringLiteral("alsa output"), QString());
    s.replace(QStringLiteral("audio input"), QString());
    s.replace(QStringLiteral("audio output"), QString());
    s.replace(QStringLiteral("analog stereo"), QString());
    s.replace(QStringLiteral("input"), QString());
    s.replace(QStringLiteral("output"), QString());
    s.replace(QStringLiteral("source"), QString());
    s.replace(QStringLiteral("sink"), QString());
    s.replace(QStringLiteral("monitor"), QString());

    return s.simplified();
}

bool sameBackendFamily(const AudioDeviceInfo &input, const AudioDeviceInfo &output)
{
    const QString inputKey = audioParentKey(input.id);
    const QString outputKey = audioParentKey(output.id);

    if (!inputKey.isEmpty() && !outputKey.isEmpty() && inputKey == outputKey) {
        return true;
    }

    return false;
}

bool supportsDesiredFormat(const QAudioDevice &device, int sampleRate, int channels)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    return device.isFormatSupported(format);
}

AudioDevicePair scorePair(
    const AudioDeviceInfo &input,
    const AudioDeviceInfo &output,
    int sampleRate,
    int channels)
{
    AudioDevicePair pair;
    pair.input = input.device;
    pair.output = output.device;
    pair.inputId = input.id;
    pair.outputId = output.id;

    if (sameDisplayDevice(input, output)) {
        pair.score += 100;
        pair.reasons << QStringLiteral("same display name");
    }

    if (sameBackendFamily(input, output)) {
        pair.score += 80;
        pair.reasons << QStringLiteral("same backend family");
    }

    if (looksLikeUsbAudio(input) || looksLikeUsbAudio(output)) {
        pair.score += 60;
        pair.reasons << QStringLiteral("looks like USB audio");
    }

    if (supportsDesiredFormat(input.device, sampleRate, channels)) {
        pair.score += 20;
        pair.reasons << QStringLiteral("input supports desired format");
    }

    if (supportsDesiredFormat(output.device, sampleRate, channels)) {
        pair.score += 20;
        pair.reasons << QStringLiteral("output supports desired format");
    }

    if (looksLikeKnownUsbCodec(input, output)) {
        pair.score += 30;
        pair.reasons << QStringLiteral("known USB codec naming");
    }

    return pair;
}

QString describeAudioDevice(const QAudioDevice &device)
{
    return QStringLiteral("\"%1\" id=\"%2\"")
        .arg(device.description(), AudioDeviceResolver::deviceIdString(device));
}

QString describeCandidate(const AudioDevicePair &candidate)
{
    return QStringLiteral("score=%1 input=\"%2\" output=\"%3\" reasons=%4")
        .arg(candidate.score)
        .arg(candidate.input.description(), candidate.output.description())
        .arg(candidate.reasons.join(QStringLiteral(", ")));
}

} // namespace

QString AudioDeviceResolver::deviceIdString(const QAudioDevice &device)
{
    const QByteArray id = device.id();

    const QString asUtf8 = QString::fromUtf8(id);
    if (!asUtf8.trimmed().isEmpty()) {
        return asUtf8;
    }

    return QString::fromLatin1(id.toHex());
}

std::optional<QAudioDevice> AudioDeviceResolver::findInputByDescription(
    const QString &description,
    QString *errorMessage)
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    const QString trimmed = description.trimmed();

    for (const QAudioDevice &device : inputs) {
        if (device.description() == trimmed) {
            return device;
        }
    }

    for (const QAudioDevice &device : inputs) {
        if (device.description().contains(trimmed, Qt::CaseInsensitive)) {
            return device;
        }
    }

    if (errorMessage) {
        QStringList available;
        for (const QAudioDevice &device : inputs) {
            available << describeAudioDevice(device);
        }

        *errorMessage = QStringLiteral("Audio input device not found: \"%1\"\nAvailable inputs:\n  %2")
            .arg(trimmed, available.join(QStringLiteral("\n  ")));
    }

    return std::nullopt;
}

std::optional<QAudioDevice> AudioDeviceResolver::findOutputByDescription(
    const QString &description,
    QString *errorMessage)
{
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    const QString trimmed = description.trimmed();

    for (const QAudioDevice &device : outputs) {
        if (device.description() == trimmed) {
            return device;
        }
    }

    for (const QAudioDevice &device : outputs) {
        if (device.description().contains(trimmed, Qt::CaseInsensitive)) {
            return device;
        }
    }

    if (errorMessage) {
        QStringList available;
        for (const QAudioDevice &device : outputs) {
            available << describeAudioDevice(device);
        }

        *errorMessage = QStringLiteral("Audio output device not found: \"%1\"\nAvailable outputs:\n  %2")
            .arg(trimmed, available.join(QStringLiteral("\n  ")));
    }

    return std::nullopt;
}

std::optional<ResolvedAudioDevices> AudioDeviceResolver::resolveManual(
    const QString &inputDescription,
    const QString &outputDescription,
    QString *errorMessage)
{
    QString inputError;
    QString outputError;

    auto input = findInputByDescription(inputDescription, &inputError);
    auto output = findOutputByDescription(outputDescription, &outputError);

    if (!input || !output) {
        if (errorMessage) {
            QStringList errors;
            if (!inputError.isEmpty()) errors << inputError;
            if (!outputError.isEmpty()) errors << outputError;
            *errorMessage = errors.join(QStringLiteral("\n"));
        }

        return std::nullopt;
    }

    ResolvedAudioDevices resolved;
    resolved.input = *input;
    resolved.output = *output;
    resolved.inputId = deviceIdString(*input);
    resolved.outputId = deviceIdString(*output);
    resolved.autoSelected = false;

    return resolved;
}

std::optional<ResolvedAudioDevices> AudioDeviceResolver::resolveAutoUsbFullDuplex(
    int sampleRate,
    int channels,
    QString *errorMessage)
{
    QList<AudioDeviceInfo> inputs;
    QList<AudioDeviceInfo> outputs;

    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        AudioDeviceInfo info;
        info.device = device;
        info.description = device.description();
        info.id = deviceIdString(device);
        info.preferredFormat = device.preferredFormat();
        info.isInput = true;

        if (!isObviouslyNonRadioAudio(info)) {
            inputs << info;
        }
    }

    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        AudioDeviceInfo info;
        info.device = device;
        info.description = device.description();
        info.id = deviceIdString(device);
        info.preferredFormat = device.preferredFormat();
        info.isInput = false;

        if (!isObviouslyNonRadioAudio(info)) {
            outputs << info;
        }
    }

    QList<AudioDevicePair> candidates;

    for (const AudioDeviceInfo &input : inputs) {
        for (const AudioDeviceInfo &output : outputs) {
            AudioDevicePair pair = scorePair(input, output, sampleRate, channels);

            // Require at least one strong "same device" signal.
            if (pair.score >= 100) {
                candidates << pair;
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const AudioDevicePair &a, const AudioDevicePair &b) {
        return a.score > b.score;
    });

    if (candidates.isEmpty()) {
        if (errorMessage) {
            QStringList lines;
            lines << QStringLiteral("No suitable USB full-duplex audio device pair found.");

            lines << QStringLiteral("Candidate inputs after filtering:");
            if (inputs.isEmpty()) {
                lines << QStringLiteral("  <none>");
            } else {
                for (const AudioDeviceInfo &input : inputs) {
                    lines << QStringLiteral("  \"%1\" id=\"%2\"")
                        .arg(input.description, input.id);
                }
            }

            lines << QStringLiteral("Candidate outputs after filtering:");
            if (outputs.isEmpty()) {
                lines << QStringLiteral("  <none>");
            } else {
                for (const AudioDeviceInfo &output : outputs) {
                    lines << QStringLiteral("  \"%1\" id=\"%2\"")
                        .arg(output.description, output.id);
                }
            }

            lines << QStringLiteral("Run tci-bridge --list-audio-devices to inspect all audio devices.");
            *errorMessage = lines.join(QStringLiteral("\n"));
        }

        return std::nullopt;
    }

    if (candidates.size() > 1) {
        const int topScore = candidates[0].score;
        const int secondScore = candidates[1].score;

        if (topScore - secondScore < 30) {
            if (errorMessage) {
                QStringList lines;
                lines << QStringLiteral("Multiple plausible USB full-duplex audio device pairs found; refusing to guess.");
                for (int i = 0; i < std::min(candidates.size(), {5}); ++i) {
                    lines << QStringLiteral("  %1").arg(describeCandidate(candidates[i]));
                }

                lines << QStringLiteral("Set audio.mode=manual and configure audio.rx_device/audio.tx_device explicitly.");
                *errorMessage = lines.join(QStringLiteral("\n"));
            }

            return std::nullopt;
        }
    }

    const AudioDevicePair &best = candidates.first();

    ResolvedAudioDevices resolved;
    resolved.input = best.input;
    resolved.output = best.output;
    resolved.inputId = best.inputId;
    resolved.outputId = best.outputId;
    resolved.score = best.score;
    resolved.reasons = best.reasons;
    resolved.autoSelected = true;

    return resolved;
}