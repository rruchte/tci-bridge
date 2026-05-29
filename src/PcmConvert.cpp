#include "PcmConvert.h"

#include <QtEndian>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace
{
    qint16 clampInt16(int value)
    {
        return static_cast<qint16>(std::clamp(value, -32768, 32767));
    }

    qint16 readInt16LE(const char *p)
    {
        return qFromLittleEndian<qint16>(
            reinterpret_cast<const uchar *>(p)
        );
    }

    qint32 readInt32LE(const char *p)
    {
        return qFromLittleEndian<qint32>(
            reinterpret_cast<const uchar *>(p)
        );
    }

    float readFloat32LE(const char *p)
    {
        quint32 raw = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(p)
        );

        float out;
        static_assert(sizeof(float) == sizeof(quint32));
        memcpy(&out, &raw, sizeof(float));
        return out;
    }

    void appendInt16LE(QByteArray &out, qint16 value)
    {
        char bytes[2];
        qToLittleEndian<qint16>(
            value,
            reinterpret_cast<uchar *>(bytes)
        );
        out.append(bytes, 2);
    }

    void appendInt32LE(QByteArray &out, qint32 value)
    {
        char bytes[4];
        qToLittleEndian<qint32>(
            value,
            reinterpret_cast<uchar *>(bytes)
        );
        out.append(bytes, 4);
    }

    void appendFloat32LE(QByteArray &out, float value)
    {
        quint32 raw;
        static_assert(sizeof(float) == sizeof(quint32));
        memcpy(&raw, &value, sizeof(float));

        char bytes[4];
        qToLittleEndian<quint32>(
            raw,
            reinterpret_cast<uchar *>(bytes)
        );
        out.append(bytes, 4);
    }

    int bytesPerSample(QAudioFormat::SampleFormat format)
    {
        switch (format) {
        case QAudioFormat::UInt8:
            return 1;
        case QAudioFormat::Int16:
            return 2;
        case QAudioFormat::Int32:
        case QAudioFormat::Float:
            return 4;
        case QAudioFormat::Unknown:
        default:
            return 0;
        }
    }

    qint16 sampleToInt16(const char *p, QAudioFormat::SampleFormat format)
    {
        switch (format) {
        case QAudioFormat::UInt8: {
            const int unsignedValue = static_cast<unsigned char>(*p);
            return static_cast<qint16>((unsignedValue - 128) << 8);
        }

        case QAudioFormat::Int16:
            return readInt16LE(p);

        case QAudioFormat::Int32: {
            const qint32 value = readInt32LE(p);
            return static_cast<qint16>(value >> 16);
        }

        case QAudioFormat::Float: {
            const float value = std::clamp(readFloat32LE(p), -1.0f, 1.0f);
            return static_cast<qint16>(std::lrint(value * 32767.0f));
        }

        case QAudioFormat::Unknown:
        default:
            return 0;
        }
    }

    void appendSampleFromInt16(
        QByteArray &out,
        qint16 sample,
        QAudioFormat::SampleFormat format
    )
    {
        switch (format) {
        case QAudioFormat::UInt8: {
            const int value = (static_cast<int>(sample) >> 8) + 128;
            out.append(static_cast<char>(std::clamp(value, 0, 255)));
            break;
        }

        case QAudioFormat::Int16:
            appendInt16LE(out, sample);
            break;

        case QAudioFormat::Int32:
            appendInt32LE(out, static_cast<qint32>(sample) << 16);
            break;

        case QAudioFormat::Float:
            appendFloat32LE(out, static_cast<float>(sample) / 32768.0f);
            break;

        case QAudioFormat::Unknown:
        default:
            break;
        }
    }
}

namespace PcmConvert
{
    bool isSupportedInputFormat(const QAudioFormat &format)
    {
        if (!format.isValid())
            return false;

        if (format.channelCount() < 1 || format.channelCount() > 2)
            return false;

        return bytesPerSample(format.sampleFormat()) > 0;
    }

    bool isSupportedOutputFormat(const QAudioFormat &format)
    {
        return isSupportedInputFormat(format);
    }

    QByteArray toMonoInt16(
        const QByteArray &input,
        const QAudioFormat &format
    )
    {
        if (!isSupportedInputFormat(format))
            return {};

        const int channels = format.channelCount();
        const int bps = bytesPerSample(format.sampleFormat());
        const int frameBytes = channels * bps;

        if (frameBytes <= 0)
            return {};

        const int frameCount = input.size() / frameBytes;

        QByteArray out;
        out.reserve(frameCount * 2);

        const char *base = input.constData();

        for (int frame = 0; frame < frameCount; ++frame) {
            const char *framePtr = base + frame * frameBytes;

            if (channels == 1) {
                const qint16 s = sampleToInt16(framePtr, format.sampleFormat());
                appendInt16LE(out, s);
            } else {
                const qint16 left = sampleToInt16(
                    framePtr,
                    format.sampleFormat()
                );

                const qint16 right = sampleToInt16(
                    framePtr + bps,
                    format.sampleFormat()
                );

                const int mixed = (static_cast<int>(left) + static_cast<int>(right)) / 2;
                appendInt16LE(out, clampInt16(mixed));
            }
        }

        return out;
    }

    QByteArray monoInt16ToFormat(
        const QByteArray &monoInt16,
        const QAudioFormat &targetFormat
    )
    {
        if (!isSupportedOutputFormat(targetFormat))
            return {};

        const int channels = targetFormat.channelCount();
        const int sampleCount = monoInt16.size() / 2;

        QByteArray out;
        out.reserve(
            sampleCount *
            channels *
            bytesPerSample(targetFormat.sampleFormat())
        );

        const char *base = monoInt16.constData();

        for (int i = 0; i < sampleCount; ++i) {
            const qint16 sample = readInt16LE(base + i * 2);

            for (int ch = 0; ch < channels; ++ch) {
                appendSampleFromInt16(
                    out,
                    sample,
                    targetFormat.sampleFormat()
                );
            }
        }

        return out;
    }

    QString describeFormat(const QAudioFormat &format)
    {
        QString sampleFormat;

        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            sampleFormat = "UInt8";
            break;
        case QAudioFormat::Int16:
            sampleFormat = "Int16";
            break;
        case QAudioFormat::Int32:
            sampleFormat = "Int32";
            break;
        case QAudioFormat::Float:
            sampleFormat = "Float";
            break;
        case QAudioFormat::Unknown:
        default:
            sampleFormat = "Unknown";
            break;
        }

        return QStringLiteral("%1Hz %2ch %3")
            .arg(format.sampleRate())
            .arg(format.channelCount())
            .arg(sampleFormat);
    }
}