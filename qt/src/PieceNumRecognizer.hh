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

#include <QJsonObject>
#include "Displayer.hh"

class DisplayerToolSelect;
class NumberedDisplayerSelection;
class QPlainTextEdit;

class StickyTooltip : public QWidget {
	Q_OBJECT
public:
	StickyTooltip(const QString& text, QPoint pos);

	static int verticalPadding();

private slots:
	void onDocumentContentsChanged();
	void onFocusChanged(QWidget *old, QWidget *now);

private:
	QString m_prevText;
	QPlainTextEdit* m_edit{};
	bool m_dragging = false;
	QPoint m_dragOffset;
	QTimer* m_closeTimer{};

	void mousePressEvent(QMouseEvent* e) override;
	void mouseMoveEvent(QMouseEvent* e) override;
	void mouseReleaseEvent(QMouseEvent* e) override;
	void keyPressEvent(QKeyEvent *e) override;
};

class PieceNumRecognizer : public QObject {
	Q_OBJECT
public:
	PieceNumRecognizer(DisplayerToolSelect* tool);
	~PieceNumRecognizer();

	void setAvgPieceNumSize(QSizeF size) {
		m_avgPieceNumSize = size;
	}
	void showConfig();
	void recognizePieceNum(NumberedDisplayerSelection* sel);

private:
	DisplayerToolSelect* m_tool;
	QSizeF m_avgPieceNumSize;

	static QPair<QJsonObject, QString> sendRequest(const QUrl& endpoint, int timeoutMs, const QJsonObject* payload = nullptr, const QString* debugPath = nullptr);
	QImage prepareImage(NumberedDisplayerSelection* sel) const;
	QJsonObject prepareOcrPayload(const QImage& img) const ;
	QPair<QString, bool> sendOcrRequest(const QJsonObject& payload, const QString& debugPath) const;
};

#endif // PIECENUMRECOGNIZER_HH
