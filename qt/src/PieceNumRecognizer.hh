/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * PieceNumRecognizer.hh
 * Copyright (C) 2026 Mikalai Ananenka <ojuuji@gmail.com>
 *
 * gImageReader is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gImageReader is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PIECENUMRECOGNIZER_HH
#define PIECENUMRECOGNIZER_HH

#include <QObject>
#include "Displayer.hh"

class DisplayerToolSelect;
class NumberedDisplayerSelection;

class PieceNumRecognizer : public QObject {
	Q_OBJECT
public:
	PieceNumRecognizer(DisplayerToolSelect* tool);
	~PieceNumRecognizer();

	void setAvgPieceNumSize(QSizeF size) {
		m_avgPieceNumSize = size;
	}
	void recognizePieceNum(NumberedDisplayerSelection* sel);

private:
	DisplayerToolSelect* m_tool;
	QSizeF m_avgPieceNumSize;

	QImage prepareImage(NumberedDisplayerSelection* sel) const;
	QByteArray preparePayload(const QImage& img) const ;
	QPair<QString, QByteArray> sendRequest(const QByteArray& payload) const;
};

#endif // PIECENUMRECOGNIZER_HH
