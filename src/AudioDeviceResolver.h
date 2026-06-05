#pragma once

#include <QAudioDevice>
#include <QString>
#include <QStringList>

#include <optional>

struct ResolvedAudioDevices {
	QAudioDevice input;
	QAudioDevice output;

	QString inputId;
	QString outputId;

	int score = 0;
	QStringList reasons;

	bool autoSelected = false;
};

class AudioDeviceResolver {
public:
	static QString deviceIdString(const QAudioDevice &device);

	static std::optional<QAudioDevice> findInputByDescription(
		const QString &description,
		QString *errorMessage = nullptr);

	static std::optional<QAudioDevice> findOutputByDescription(
		const QString &description,
		QString *errorMessage = nullptr);

	static std::optional<ResolvedAudioDevices> resolveManual(
		const QString &inputDescription,
		const QString &outputDescription,
		QString *errorMessage = nullptr);

	static std::optional<ResolvedAudioDevices> resolveAutoUsbFullDuplex(
		int sampleRate,
		int channels,
		QString *errorMessage = nullptr);
};