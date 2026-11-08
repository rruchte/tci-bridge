#include "TciStreamFrame.h"
#include "TciStartup.h"
#include "TciSampleRate.h"
#include "TciModeNames.h"
#include "TciFrequencyCaps.h"

#include <cmath>
#include <algorithm>

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
    const QString stateSample =
        "rigctld: Hamlib 4.6.2 2025-02-09T21:03:50Z SHA=870364 64-bit\n"
        "1\n2\n0\n"
        "30000.000000 60000000.000000 0x420201dbf -1 -1 0x10000003 0x80000000\n"
        "0 0 0 0 0 0 0\n"
        "1810000.000000 2000000.000000 0x420201c3f 5000 200000 0x10000003 0x80000000\n"
        "3500000.000000 3800000.000000 0x420201c3f 5000 200000 0x10000003 0x80000000\n"
        "0 0 0 0 0 0 0\n"
        "0xd9e 10\n";
    // The raw rigctld TCP response need not include the user-facing
    // "rigctld:" version banner printed by some rigctl clients.
    const QString stateWithoutBanner = stateSample.section('\n', 1);
    const auto rawLimits = TciFrequencyCaps::fromDumpState(stateWithoutBanner);
    if (!check(rawLimits.valid() && rawLimits.minHz == 30000 &&
               rawLimits.maxHz == 60000000,
               "parse raw dump_state without optional rigctl banner"))
        return 1;

    const auto ftdxLimits = TciFrequencyCaps::fromDumpState(stateSample);
    if (!check(ftdxLimits.valid() &&
               ftdxLimits.minHz == 30000 && ftdxLimits.maxHz == 60000000,
               "discover FTDX-10 RX range without including TX limits"))
        return 1;

    const QString disjointState =
        "rigctld: Hamlib\n1\n2\n0\n"
        "30000 50000 0x1 -1 -1 0x3 0x0\n"
        "70000 90000 0x1 -1 -1 0x3 0x0\n"
        "0 0 0 0 0 0 0\n"
        "70000 80000 0x1 5000 10000 0x3 0x0\n"
        "0 0 0 0 0 0 0\n";
    if (!check(!TciFrequencyCaps::fromDumpState(disjointState).valid(),
               "omit VFO limits for disconnected RX bands"))
        return 1;
    const QString adjacentState =
        "rigctld: Hamlib\n1\n2\n0\n"
        "50000 90000 0x1 -1 -1 0x3 0x0\n"
        "30000 50000 0x1 -1 -1 0x3 0x0\n"
        "0 0 0 0 0 0 0\n"
        "0 0 0 0 0 0 0\n";
    const auto adjacent = TciFrequencyCaps::fromDumpState(adjacentState);
    if (!check(adjacent.valid() &&
               adjacent.minHz == 30000 && adjacent.maxHz == 90000,
               "merge contiguous RX frequency ranges"))
        return 1;
    if (!check(!TciFrequencyCaps::fromDumpState(
                   stateSample.section("0 0 0 0 0 0 0", 0, 1)).valid(),
               "reject truncated dump_state RX/TX sections"))
        return 1;

    const QString ftdx10Modes =
        "AM CW USB LSB RTTY FM CWR RTTYR PKTLSB PKTUSB "
        "FM-D FMN AMN PKTFMN\nRPRT 0";
    const QStringList parsedModes =
        TciModeNames::parseHamlibSupportedModes(ftdx10Modes);
    const QStringList expectedModes = {
        "AM", "CW", "USB", "LSB", "RTTY", "FM", "CWR", "RTTYR",
        "DIGL", "DIGU", "FM-D", "FMN", "AMN", "PKTFMN"
    };
    if (!check(parsedModes == expectedModes,
               "FTDX-10 mode discovery preserves distinct modes"))
        return 1;

    if (!check(TciModeNames::toHamlib("digu") == "PKTUSB" &&
               TciModeNames::toHamlib("DIGL") == "PKTLSB" &&
               TciModeNames::fromHamlib("pktusb") == "DIGU" &&
               TciModeNames::fromHamlib("PKTLSB") == "DIGL",
               "round-trip known Hamlib/TCI digital-sideband aliases"))
        return 1;

    if (!check(TciModeNames::fromHamlib("FMN") == "FMN" &&
               TciModeNames::toHamlib("FM-D") == "FM-D" &&
               TciModeNames::fromHamlib("AMN") == "AMN" &&
               TciModeNames::toHamlib("PKTFMN") == "PKTFMN",
               "do not collapse distinct radio-specific modes"))
        return 1;

    if (!check(TciModeNames::parseHamlibSupportedModes(
                    "USB FMN NEWMODE\\nRPRT 0").size() == 0,
               "reject malformed Hamlib discovery framing"))
        return 1;
    if (!check(TciModeNames::parseHamlibSupportedModes(
                   "USB FMN NEWMODE\nRPRT 0").size() == 3,
               "accept valid future Hamlib mode names"))
        return 1;
    if (!check(TciModeNames::parseHamlibSupportedModes(
                   "USB FMN\nRPRT -1").isEmpty() &&
               TciModeNames::parseHamlibSupportedModes(
                   "USB FMN\nRPRT 0\nGARBAGE").isEmpty(),
               "reject failed or extra Hamlib status lines"))
        return 1;

    const QStringList rxOnlyStartup = TciStartup::messages(false);
    const QStringList txStartup = TciStartup::messages(true);
    if (!check(rxOnlyStartup.contains("receive_only:true;") &&
               txStartup.contains("receive_only:false;") &&
               rxOnlyStartup.last() == "ready;" &&
               txStartup.last() == "ready;" &&
               rxOnlyStartup.contains("trx_count:1;") &&
               txStartup.contains("channels_count:1;"),
               "startup reflects TX permission and stable bridge topology"))
        return 1;

    for (const QString &message : txStartup) {
        if (!check(!message.startsWith("vfo_limits:") &&
                   !message.startsWith("if_limits:") &&
                   !message.startsWith("modulations_list:"),
                   "startup does not claim unsupported radio capabilities"))
            return 1;
    }

    if (!check(TciStream::DEFAULT_RX_SAMPLE_RATE == 48000 &&
               TciStream::DEFAULT_RX_SAMPLE_TYPE == TciStream::FLOAT32 &&
               TciStream::DEFAULT_RX_CHANNELS == 2 &&
               TciStream::DEFAULT_RX_SAMPLES_PER_FRAME == 2048,
               "TCI 2.1 RX defaults"))
        return 1;


    // Native 48-kHz frames must remain bit-identical.
    TciSampleRate::RxDecimator nativeRate;
    if (!check(nativeRate.configure(48000), "configure 48-kHz RX"))
        return 1;
    QByteArray source(48000 * 2 / 10, char(0));
    for (int i = 0; i < source.size(); i += 2) {
        source[i] = char((i / 2) & 0xff);
        source[i + 1] = char(((i / 2) >> 8) & 0xff);
    }
    if (!check(nativeRate.process(source) == source, "48-kHz RX passthrough"))
        return 1;

    for (quint32 rate : { 8000U, 12000U, 24000U }) {
        TciSampleRate::RxDecimator whole, chunks;
        if (!check(whole.configure(rate) && chunks.configure(rate),
                   "configure supported lower RX rate"))
            return 1;
        const QByteArray expected = whole.process(source);
        QByteArray actual;
        // Chunk boundaries are deliberately not aligned to the ratio.
        const int splits[] = { 102, 370, 2046, 8192, int(source.size()) };
        int previous = 0;
        for (int boundary : splits) {
            actual.append(chunks.process(source.mid(previous, boundary - previous)));
            previous = boundary;
        }
        if (!check(actual == expected, "resampler preserves continuity across chunks"))
            return 1;
        if (!check(actual.size() == source.size() * int(rate) / 48000,
                   "correct lower-rate output sample count"))
            return 1;
    }

    // TX upsampling must preserve 48-kHz bytes and carry filter history
    // across arbitrarily split WebSocket frames.
    TciSampleRate::TxInterpolator nativeTx;
    if (!check(nativeTx.configure(48000) &&
               nativeTx.process(source) == source,
               "48-kHz TX remains byte-for-byte identical"))
        return 1;

    for (quint32 rate : { 8000U, 12000U, 24000U }) {
        TciSampleRate::TxInterpolator continuous, split;
        if (!check(continuous.configure(rate) && split.configure(rate),
                   "configure supported TX rate"))
            return 1;

        QByteArray lowerRate;
        lowerRate.reserve(rate / 10 * 2);
        for (quint32 i = 0; i < rate / 10; ++i) {
            const auto sample = static_cast<qint16>(std::lround(
                12000.0 * std::sin(2.0 * 3.14159265358979323846 *
                                   1000.0 * i / rate)));
            const quint16 bits = static_cast<quint16>(sample);
            lowerRate.append(char(bits & 0xff));
            lowerRate.append(char((bits >> 8) & 0xff));
        }

        const QByteArray whole = continuous.process(lowerRate);
        QByteArray pieces;
        const int boundaries[] = { 38, 114, 626, int(lowerRate.size()) };
        int previous = 0;
        for (int boundary : boundaries) {
            pieces.append(split.process(
                lowerRate.mid(previous, boundary - previous)));
            previous = boundary;
        }
        if (!check(whole == pieces, "TX resampler chunk continuity"))
            return 1;
        if (!check(whole.size() == lowerRate.size() * 48000 / int(rate),
                   "TX resampler output count"))
            return 1;

        // Verify that the nonzero waveform survives the interpolation.
        int peak = 0;
        for (qsizetype i = 500; i < whole.size(); i += 2) {
            const auto *p = reinterpret_cast<const unsigned char *>(
                whole.constData() + i);
            const qint16 sample = static_cast<qint16>(
                quint16(p[0]) | (quint16(p[1]) << 8));
            peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        if (!check(peak > 9000 && peak < 14000,
                   "TX interpolator preserves expected tone amplitude"))
            return 1;
    }

    if (!check(!TciSampleRate::isSupported(44100), "reject unsupported rate"))
        return 1;

    // 9-kHz input at 48 kHz must be suppressed before conversion to 12 kHz,
    // where it would otherwise fold into the audible band as a 3-kHz alias.
    TciSampleRate::RxDecimator filtered;
    if (!check(filtered.configure(12000), "configure anti-alias test"))
        return 1;
    QByteArray tone;
    tone.reserve(4800 * 2);
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 4800; ++i) {
        const auto sample = static_cast<qint16>(
            std::lround(12000.0 * std::sin(2.0 * pi * 9000.0 * i / 48000.0)));
        const quint16 bits = static_cast<quint16>(sample);
        tone.append(char(bits & 0xff));
        tone.append(char((bits >> 8) & 0xff));
    }
    const QByteArray filteredOutput = filtered.process(tone);
    int peak = 0;
    for (qsizetype i = 200; i < filteredOutput.size(); i += 2) {
        const auto *p = reinterpret_cast<const unsigned char *>(
            filteredOutput.constData() + i);
        const auto value = static_cast<qint16>(
            quint16(p[0]) | (quint16(p[1]) << 8));
        peak = std::max(peak, std::abs(static_cast<int>(value)));
    }
    if (!check(peak < 100, "12-kHz RX anti-alias filtering"))
        return 1;

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

    // For 48-kHz stereo defaults, Stream.length is total scalar
    // sample values across channels: 1024 time samples * 2 = 2048.
    const QByteArray defaultMono(1024 * 2, char(0));
    const QByteArray defaultStereo = TciStream::convertRxMonoInt16(
        defaultMono, TciStream::DEFAULT_RX_SAMPLE_TYPE,
        TciStream::DEFAULT_RX_CHANNELS);
    const auto defaultFrame = TciStream::parseFrame(
        TciStream::makeRxAudioFrame(
            defaultStereo, 0, TciStream::DEFAULT_RX_SAMPLE_RATE,
            TciStream::DEFAULT_RX_SAMPLE_TYPE,
            TciStream::DEFAULT_RX_CHANNELS, 0));
    if (!check(defaultFrame.valid &&
               defaultFrame.sampleCount == 2048 &&
               defaultFrame.channels == 2 &&
               defaultFrame.payload.size() == 2048 * 4,
               "48-kHz stereo float32 default packet size and count"))
        return 1;

    const QByteArray chrono = TciStream::makeTxChronoFrame(
        0, 48000, TciStream::FLOAT32, 1, 512, 42);
    const auto parsedChrono = TciStream::parseFrame(chrono);
    if (!check(chrono.size() == 64 && parsedChrono.valid &&
               parsedChrono.streamType == TciStream::TX_CHRONO &&
               parsedChrono.sampleCount == 512 &&
               parsedChrono.sampleType == TciStream::FLOAT32 &&
               parsedChrono.count == 42 &&
               parsedChrono.payload.isEmpty(),
               "header-only TX_CHRONO"))
        return 1;

    if (!check(TciStream::convertRxMonoInt16(
                   pcm, TciStream::INT16, 1) == pcm,
               "mono int16 conversion preserves bytes"))
        return 1;


    // Native mono int16 is preserved byte-for-byte.
    const auto native = TciStream::parseFrame(TciStream::makeRxAudioFrame(
        pcm, 0, 48000, TciStream::INT16, 1, 1));
    auto txNative = native;
    txNative.streamType = TciStream::TX_AUDIO_STREAM;
    if (!check(TciStream::convertTxToMonoInt16(txNative) == pcm,
               "native mono int16 TX is unchanged"))
        return 1;

    for (auto type : { TciStream::INT16, TciStream::INT24,
                       TciStream::INT32, TciStream::FLOAT32 }) {
        for (quint32 channels : { 1U, 2U }) {
            const QByteArray payload = TciStream::convertRxMonoInt16(pcm, type, channels);
            auto frame = TciStream::parseFrame(TciStream::makeRxAudioFrame(
                payload, 0, 48000, type, channels, 3));
            frame.streamType = TciStream::TX_AUDIO_STREAM;
            if (!check(frame.valid && TciStream::convertTxToMonoInt16(frame) == pcm,
                       "TX format/channel roundtrip"))
                return 1;
        }
    }

    auto malformed = txNative;
    malformed.sampleCount += 1;
    if (!check(TciStream::convertTxToMonoInt16(malformed).isEmpty(),
               "reject mismatched TX frame length"))
        return 1;

    qInfo() << "TCI stream framing tests passed";
    return 0;
}
