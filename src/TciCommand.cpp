#include "TciCommand.h"

TciCommand TciCommand::parse(const QString &message)
{
	TciCommand result;
	result.raw = message.trimmed();

	if (result.raw.isEmpty())
		return result;

	QString s = result.raw;

	if (s.endsWith(';'))
		s.chop(1);

	const int colon = s.indexOf(':');

	if (colon < 0) {
		result.name = s.trimmed().toLower();
		result.valid = !result.name.isEmpty();
		return result;
	}

	result.name = s.left(colon).trimmed().toLower();

	const QString argString = s.mid(colon + 1).trimmed();

	if (!argString.isEmpty()) {
		const auto parts = argString.split(',', Qt::KeepEmptyParts);
		for (const QString &part : parts)
			result.args << part.trimmed();
	}

	result.valid = !result.name.isEmpty();
	return result;
}