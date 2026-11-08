#include "TciStreamFrame.h"

#include <cstring>

namespace
{
    constexpr int TCI_STREAM_HEADER_UINT32_COUNT = 16;
    constexpr int TCI_STREAM_HEADER_BYTES = TCI_STREAM_HEADER_UINT32_COUNT * 4;

    void appendU32LE(QByteArray &out, quint32 value)
    {
        out.append(static_cast<char>(value & 0xff));
        out.append(static_cast<char>((value >> 8) & 0xff));
        out.append(static_cast<char>((value >> 16) & 0xff));
        out.append(static_cast<char>((value >> 24) & 0xff));
    }

    quint32 readU32LE(const QByteArray &in, int offset)
    {
        const auto b0 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset)));
        const auto b1 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 1)));
        const auto b2 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 2)));
        const auto b3 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 3)));

        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }
}

namespace TciStream
{
    QByteArray convertRxMonoInt16(const QByteArray &monoPcm,
                                   quint32 sampleType, quint32 channels)
    {
        if (channels != 1 && channels != 2)
            return {};
        if (monoPcm.size() % 2 != 0)
            return {};
        const int bps = bytesPerSample(sampleType);
        if (bps == 0)
            return {};

        QByteArray result;
        result.reserve(monoPcm.size() / 2 * static_cast<int>(channels) * bps);
        const auto *data = reinterpret_cast<const unsigned char *>(monoPcm.constData());
        for (qsizetype i = 0; i < monoPcm.size(); i += 2) {
            const quint16 bits = quint16(data[i]) | (quint16(data[i + 1]) << 8);
            const qint16 sample = static_cast<qint16>(bits);

            for (quint32 channel = 0; channel < channels; ++channel) {
                if (sampleType == INT16) {
                    result.append(static_cast<char>(bits & 0xff));
                    result.append(static_cast<char>((bits >> 8) & 0xff));
                } else if (sampleType == FLOAT32) {
                    const float value = static_cast<float>(sample) / 32768.0f;
                    quint32 encoded = 0;
                    static_assert(sizeof(value) == sizeof(encoded), "float32 required");
                    std::memcpy(&encoded, &value, sizeof(encoded));
                    appendU32LE(result, encoded);
                } else {
                    // Signed 16-bit audio is extended to 24/32 bits,
                    // preserving its amplitude and sign.
                    const qint64 expanded = static_cast<qint64>(sample) *
                        (sampleType == INT24 ? 256LL : 65536LL);
                    const quint32 encoded = static_cast<quint32>(expanded);
                    result.append(static_cast<char>(encoded & 0xff));
                    result.append(static_cast<char>((encoded >> 8) & 0xff));
                    result.append(static_cast<char>((encoded >> 16) & 0xff));
                    if (sampleType == INT32)
                        result.append(static_cast<char>((encoded >> 24) & 0xff));
                }
            }
        }
        return result;
    }

    int bytesPerSample(quint32 sampleType)
    {
        switch (sampleType) {
        case INT16:
            return 2;
        case INT24:
            return 3;
        case INT32:
        case FLOAT32:
            return 4;
        default:
            return 0;
        }
    }

    QByteArray makeRxAudioFrame(
        const QByteArray &pcm,
        quint32 receiver,
        quint32 sampleRate,
        quint32 sampleType,
        quint32 channels,
        quint32 count
    )
    {
        const int bps = bytesPerSample(sampleType);
        const quint32 sampleCount =
            bps > 0 ? static_cast<quint32>(pcm.size() / bps) : 0;

        QByteArray frame;
        frame.reserve(TCI_STREAM_HEADER_BYTES + pcm.size());

        appendU32LE(frame, receiver);
        appendU32LE(frame, sampleRate);
        appendU32LE(frame, sampleType);
        appendU32LE(frame, 0);
        appendU32LE(frame, 0);
        appendU32LE(frame, sampleCount);
        appendU32LE(frame, RX_AUDIO_STREAM);
        appendU32LE(frame, channels);

        appendU32LE(frame, count);
        for (int i = 0; i < 7; ++i)
            appendU32LE(frame, 0);

        frame.append(pcm);
        return frame;
    }

    QByteArray makeTxChronoFrame(quint32 receiver, quint32 sampleRate,
                                  quint32 sampleType, quint32 channels,
                                  quint32 samples, quint32 count)
    {
        QByteArray frame;
        frame.reserve(TCI_STREAM_HEADER_BYTES);
        appendU32LE(frame, receiver);
        appendU32LE(frame, sampleRate);
        appendU32LE(frame, sampleType);
        appendU32LE(frame, 0); // codec
        appendU32LE(frame, 0); // crc
        appendU32LE(frame, samples);
        appendU32LE(frame, TX_CHRONO);
        appendU32LE(frame, channels);
        appendU32LE(frame, count);
        for (int i = 0; i < 7; ++i)
            appendU32LE(frame, 0);
        return frame;
    }

    StreamFrame parseFrame(const QByteArray &frame)
    {
        StreamFrame result;

        if (frame.size() < TCI_STREAM_HEADER_BYTES)
            return result;

        result.receiver = readU32LE(frame, 0);
        result.sampleRate = readU32LE(frame, 4);
        result.sampleType = readU32LE(frame, 8);
        result.codec = readU32LE(frame, 12);
        result.crc = readU32LE(frame, 16);
        result.sampleCount = readU32LE(frame, 20);
        result.streamType = readU32LE(frame, 24);
        result.channels = readU32LE(frame, 28);
        result.count = readU32LE(frame, 32);
        result.payload = frame.mid(TCI_STREAM_HEADER_BYTES);

        // TX_CHRONO requests samples but carries no PCM payload.
        if (result.streamType == TX_CHRONO) {
            result.valid = true;
            return result;
        }

        const int bps = bytesPerSample(result.sampleType);

        if (bps <= 0)
            return result;

        const qsizetype expectedMin =
            static_cast<qsizetype>(result.sampleCount) *
            static_cast<qsizetype>(bps);

        if (result.payload.size() < expectedMin)
            return result;

        result.valid = true;
        return result;
    }
}