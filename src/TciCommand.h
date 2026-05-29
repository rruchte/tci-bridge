#pragma once

#include <QString>
#include <QStringList>

struct TciCommand
{
	QString name;
	QStringList args;
	QString raw;

	bool valid = false;

	static TciCommand parse(const QString &message);
};