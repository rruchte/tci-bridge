#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <array>

// The audio backend produces 48-kHz mono signed int16 samples.
// Downsample per TCI client, preserving filter and phase state across chunks.
namespace TciSampleRate
{
    bool isSupported(quint32 rate);

    // Converts supported lower-rate mono int16 TX audio into the native
    // 48-kHz mono int16 stream consumed by the audio backend.
    // FIR history and interpolation phase persist between WebSocket frames.
    class TxInterpolator
    {
    public:
        bool configure(quint32 inputRate);
        void reset();
        QByteArray process(const QByteArray &monoInt16);
        quint32 inputRate() const { return input_rate_; }

    private:
        static constexpr int Taps = 33;
        static constexpr int MaxFactor = 6;
        quint32 input_rate_ = 48000;
        int factor_ = 1;
        std::array<std::array<double, Taps>, MaxFactor> coefficients_{};
        std::array<qint16, Taps> history_{};
        int head_ = 0;
    };

    class RxDecimator
    {
    public:
        bool configure(quint32 outputRate);
        void reset();
        QByteArray process(const QByteArray &mono48kInt16);
        quint32 outputRate() const { return output_rate_; }

    private:
        static constexpr int Taps = 63;
        quint32 output_rate_ = 48000;
        int factor_ = 1;
        std::array<double, Taps> coefficients_{};
        std::array<qint16, Taps> history_{};
        int head_ = 0;
        quint64 input_count_ = 0;
    };
}
