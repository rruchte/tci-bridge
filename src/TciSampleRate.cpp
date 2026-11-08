#include "TciSampleRate.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double Pi = 3.14159265358979323846;

inline qint16 read16(const unsigned char *p)
{
    return static_cast<qint16>(quint16(p[0]) | (quint16(p[1]) << 8));
}

inline void append16(QByteArray &output, qint16 sample)
{
    const quint16 bits = static_cast<quint16>(sample);
    output.append(static_cast<char>(bits & 0xff));
    output.append(static_cast<char>((bits >> 8) & 0xff));
}
}

namespace TciSampleRate
{
bool isSupported(quint32 rate)
{
    return rate == 8000 || rate == 12000 || rate == 24000 || rate == 48000;
}

bool TxInterpolator::configure(quint32 inputRate)
{
    if (!isSupported(inputRate))
        return false;

    input_rate_ = inputRate;
    factor_ = 48000 / static_cast<int>(inputRate);
    for (auto &phase : coefficients_)
        phase.fill(0.0);

    if (factor_ != 1) {
        // For each output phase, reconstruct a delayed input-time sample.
        // The filter suppresses images above the input Nyquist frequency.
        // The delay is 16 source samples, identical for every output phase.
        constexpr double delay = (Taps - 1) / 2.0;
        constexpr double cutoff = 0.45;
        for (int phase = 0; phase < factor_; ++phase) {
            const double fraction = static_cast<double>(phase) / factor_;
            double sum = 0.0;
            for (int tap = 0; tap < Taps; ++tap) {
                const double displacement = tap - delay + fraction;
                const double kernel = displacement == 0.0
                    ? 2.0 * cutoff
                    : std::sin(2.0 * Pi * cutoff * displacement) /
                      (Pi * displacement);
                const double window = 0.42
                    - 0.5 * std::cos(2.0 * Pi * tap / (Taps - 1))
                    + 0.08 * std::cos(4.0 * Pi * tap / (Taps - 1));
                coefficients_[phase][tap] = kernel * window;
                sum += coefficients_[phase][tap];
            }
            for (double &coefficient : coefficients_[phase])
                coefficient /= sum;
        }
    }

    reset();
    return true;
}

void TxInterpolator::reset()
{
    history_.fill(0);
    head_ = 0;
}

QByteArray TxInterpolator::process(const QByteArray &pcm)
{
    if (pcm.size() % 2)
        return {};

    if (factor_ == 1)
        return pcm;

    QByteArray output;
    output.reserve((pcm.size() / 2) * factor_ * 2);
    const auto *input = reinterpret_cast<const unsigned char *>(pcm.constData());

    for (qsizetype position = 0; position < pcm.size(); position += 2) {
        history_[head_] = read16(input + position);
        head_ = (head_ + 1) % Taps;

        for (int phase = 0; phase < factor_; ++phase) {
            double value = 0.0;
            for (int tap = 0; tap < Taps; ++tap) {
                const int index = (head_ + Taps - 1 - tap) % Taps;
                value += coefficients_[phase][tap] * history_[index];
            }
            const auto rounded = static_cast<int>(std::lround(value));
            append16(output, static_cast<qint16>(
                std::clamp(rounded, -32768, 32767)));
        }
    }

    return output;
}

bool RxDecimator::configure(quint32 outputRate)
{
    if (!isSupported(outputRate))
        return false;

    output_rate_ = outputRate;
    factor_ = 48000 / static_cast<int>(outputRate);
    coefficients_.fill(0.0);

    if (factor_ != 1) {
        // Windowed-sinc anti-alias filter. Normalized cutoff is 45% of
        // the output sample rate (input Nyquist is 0.5 cycles/sample).
        const double cutoff = 0.45 / factor_;
        double sum = 0.0;
        for (int k = 0; k < Taps; ++k) {
            const double x = k - (Taps - 1) / 2.0;
            const double sinc = x == 0.0
                ? 2.0 * cutoff
                : std::sin(2.0 * Pi * cutoff * x) / (Pi * x);
            const double window = 0.42
                - 0.5 * std::cos(2.0 * Pi * k / (Taps - 1))
                + 0.08 * std::cos(4.0 * Pi * k / (Taps - 1));
            coefficients_[k] = sinc * window;
            sum += coefficients_[k];
        }
        for (double &coefficient : coefficients_)
            coefficient /= sum;
    }

    reset();
    return true;
}

void RxDecimator::reset()
{
    history_.fill(0);
    head_ = 0;
    input_count_ = 0;
}

QByteArray RxDecimator::process(const QByteArray &pcm)
{
    if (pcm.size() % 2)
        return {};

    if (factor_ == 1)
        return pcm;

    QByteArray output;
    output.reserve((pcm.size() / 2 / factor_ + 1) * 2);
    const auto *input = reinterpret_cast<const unsigned char *>(pcm.constData());

    for (qsizetype pos = 0; pos < pcm.size(); pos += 2) {
        history_[head_] = read16(input + pos);
        head_ = (head_ + 1) % Taps;
        ++input_count_;

        if (input_count_ % static_cast<quint64>(factor_) != 0)
            continue;

        double sample = 0.0;
        // head_ points to the oldest entry, immediately after the newest.
        for (int k = 0; k < Taps; ++k) {
            const int index = (head_ + Taps - 1 - k) % Taps;
            sample += coefficients_[k] * history_[index];
        }

        const auto rounded = static_cast<int>(std::lround(sample));
        append16(output, static_cast<qint16>(
            std::clamp(rounded, -32768, 32767)));
    }

    return output;
}
}
