#include "TciStreamFrame.h"

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
            return 2;
        }
    }

    QByteArray makeRxAudioFrame(
        const QByteArray &pcm,
        quint32 receiver,
        quint32 sampleRate,
        quint32 sampleType,
        quint32 channels
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

        for (int i = 0; i < 8; ++i)
            appendU32LE(frame, 0);

        frame.append(pcm);
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
        result.payload = frame.mid(TCI_STREAM_HEADER_BYTES);

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