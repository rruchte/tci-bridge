#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <array>

// The audio backend produces 48-kHz mono signed int16 samples.
// Downsample per TCI client, preserving filter and phase state across chunks.
namespace TciSampleRate
{
    bool isSupported(quint32 rate);

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
