#include "TciStreamFrame.h"

namespace
{
    constexpr int TCI_STREAM_HEADER_UINT32_COUNT = 16;
    constexpr int TCI_STREAM_HEADER_BYTES = TCI_STREAM_HEADER_UINT32_COUNT * 4;

    quint32 readU32LE(const QByteArray &in, int offset)
    {
        const auto b0 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset)));
        const auto b1 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 1)));
        const auto b2 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 2)));
        const auto b3 = static_cast<quint32>(static_cast<unsigned char>(in.at(offset + 3)));

        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    void appendU32LE(QByteArray &out, quint32 value)
    {
        out.append(static_cast<char>(value & 0xff));
        out.append(static_cast<char>((value >> 8) & 0xff));
        out.append(static_cast<char>((value >> 16) & 0xff));
        out.append(static_cast<char>((value >> 24) & 0xff));
    }
}

namespace TciStream
{
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

        result.valid = true;
        return result;
    }

    QByteArray makeTxAudioFrame(
        const QByteArray &pcm,
        quint32 receiver,
        quint32 sampleRate,
        quint32 sampleType,
        quint32 channels[$currClientKey]
    )
    {
        const quint32 bytesPerSample = sampleType == INT16 ? 2 : 2;
        const quint32 sampleCount =
            channels > 0 && bytesPerSample > 0
                ? static_cast<quint32>(pcm.size()) / bytesPerSample / channels
                : 0;

        QByteArray frame;
        frame.reserve(TCI_STREAM_HEADER_BYTES + pcm.size());

        appendU32LE(frame, receiver);
        appendU32LE(frame, sampleRate);
        appendU32LE(frame, sampleType);
        appendU32LE(frame, 0); // codec
        appendU32LE(frame, 0); // crc
        appendU32LE(frame, sampleCount);
        appendU32LE(frame, TX_AUDIO_STREAM);
        appendU32LE(frame, channels);

        for (int i = 0; i < 8; ++i)
            appendU32LE(frame, 0);

        frame.append(pcm);
        return frame;
    }
}