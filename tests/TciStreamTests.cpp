#include "TciStreamFrame.h"

#include <QByteArray>
#include <QDebug>

namespace
{
bool check(bool ok, const char *name)
{
    if (!ok)
        qCritical() << "FAILED:" << name;
    return ok;
}
}

int main()
{
    // Two mono samples, one positive and one negative.
    QByteArray pcm;
    pcm.append(char(0x00));
    pcm.append(char(0x40)); // +16384
    pcm.append(char(0x00));
    pcm.append(char(0xC0)); // -16384

    const QByteArray stereoFloat = TciStream::convertRxMonoInt16(
        pcm, TciStream::FLOAT32, 2);
    if (!check(stereoFloat.size() == 16, "stereo float32 conversion size"))
        return 1;

    const QByteArray rx = TciStream::makeRxAudioFrame(
        stereoFloat, 0, 48000, TciStream::FLOAT32, 2, 7);
    if (!check(rx.size() == 80, "64-byte RX header"))
        return 1;

    const auto parsedRx = TciStream::parseFrame(rx);
    if (!check(parsedRx.valid &&
               parsedRx.streamType == TciStream::RX_AUDIO_STREAM &&
               parsedRx.count == 7 && parsedRx.channels == 2 &&
               parsedRx.sampleCount == 4,
               "RX counter, channels and scalar sample count"))
        return 1;

    const QByteArray chrono = TciStream::makeTxChronoFrame(
        0, 48000, TciStream::INT16, 1, 512, 42);
    const auto parsedChrono = TciStream::parseFrame(chrono);
    if (!check(chrono.size() == 64 && parsedChrono.valid &&
               parsedChrono.streamType == TciStream::TX_CHRONO &&
               parsedChrono.sampleCount == 512 && parsedChrono.count == 42 &&
               parsedChrono.payload.isEmpty(),
               "header-only TX_CHRONO"))
        return 1;

    if (!check(TciStream::convertRxMonoInt16(
                   pcm, TciStream::INT16, 1) == pcm,
               "mono int16 conversion preserves bytes"))
        return 1;

    qInfo() << "TCI stream framing tests passed";
    return 0;
}
