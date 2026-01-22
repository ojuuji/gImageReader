/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * PieceNumRecognizer.cc
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

#include "PieceNumRecognizer.hh"
#include "ConfigSettings.hh"
#include "DisplayerToolSelect.hh"
#include "MainWindow.hh"
#include "OutputEditor.hh"

#include <QBuffer>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>


static QString DEFAULT_OLLAMA_MODEL = "llama3.2-vision";
static QString DEFAULT_OLLAMA_PROMPT = R"(
You are extracting structured data from a CaDA "bill of materials" page.

The image contains:
- A piece image.
- A magenta outlined box overlaid on top of the piece image. This box is only a selection indicator and must be ignored.
- Directly below this piece image, there are exactly two lines of text:
  1. Piece quantity
  2. Piece SKU

Important:
- The image may contain other SKUs, quantities, or text elsewhere. Ignore all of them.
- Only the two lines of text directly below the selected piece image are relevant.

Extract ONLY:
- The piece quantity, which appears in one of these exact formats: "<number>x" or "x<number>" (examples: "14x", "2x", "x67", "x1").
- The piece SKU, which is the other line of text. It contains at least 8 characters, only uppercase Latin letters and digits.

Output rules:
- Use exactly this JSON structure: {"<piece_SKU>":"<piece_quantity>"}
- Do not add explanations, reasoning, comments, or any text outside the JSON structure.
- Do not infer or guess missing information. If either value is unreadable, output an empty string for that field.

)";
static QString DEFAULT_OLLAMA_REGEX = "\"(\\S+)\"\\s*:\\s*\"(\\S+)\"";

PieceNumRecognizer::PieceNumRecognizer(DisplayerToolSelect* tool)
	: QObject(tool), m_tool(tool) {
	ADD_SETTING(VarSetting<QString> ("ollamaModel", DEFAULT_OLLAMA_MODEL));
	ADD_SETTING(VarSetting<QString> ("ollamaPrompt", DEFAULT_OLLAMA_PROMPT));
	ADD_SETTING(VarSetting<QString> ("ollamaRegex", DEFAULT_OLLAMA_REGEX));
	ADD_SETTING(VarSetting<bool> ("ollamaDebug", false));
}

PieceNumRecognizer::~PieceNumRecognizer() {
}

void PieceNumRecognizer::recognizePieceNum(NumberedDisplayerSelection* sel) {
	if (m_avgPieceNumSize.isEmpty()) {
		QMessageBox::warning(MAIN, _("Recognition errors"), _("You must set average piece num size first."));
		return;
	}

	QImage img = prepareImage(sel);
	QByteArray payload = preparePayload(img);

	auto [response, raw] = sendRequest(payload);
	if (!response.isEmpty()) {
		MAIN->getOutputEditor()->appendText(response + "\n");
	}

	bool debug = ConfigSettings::get<VarSetting<bool>> ("ollamaDebug")->getValue();
	if (debug) {
		QString basePath = QDir(QDir::tempPath()).filePath("gImageRearerLastOllama");
		img.save(basePath + "Request.png");

		QFile req(basePath + "Request.json");
		if (req.open(QIODevice::WriteOnly)) {
			req.write(payload);
			req.close();
		}

		QFile resp(basePath + "Response.bin");
		if (resp.open(QIODevice::WriteOnly)) {
			resp.write(raw);
			resp.close();
		}

		MAIN->showStatus(_("Saved Ollama communication as %1Re*.*").arg(basePath));
	}
}

QImage PieceNumRecognizer::prepareImage(NumberedDisplayerSelection* sel) const {
	QRectF pieceRect = sel->rect();
	pieceRect.setTop(pieceRect.top() + pieceRect.height() / 2.0);

	QPointF offset(m_avgPieceNumSize.width() / 2.0, 1.0);
	qreal minWidth = qMax(m_avgPieceNumSize.width(), pieceRect.width());
	QRectF exportRect = pieceRect;
	exportRect.adjust(-offset.x(), -offset.y(), minWidth * 1.5 - exportRect.width(), 1.5 * m_avgPieceNumSize.height());
	QImage img = m_tool->getDisplayer()->getImage(exportRect);

	QRectF boxRect(offset, pieceRect.size());
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, false);
	p.fillRect(boxRect, QColor(255, 0, 255, 63));
	p.setBrush(Qt::NoBrush);
	p.setPen(QColor(255, 0, 255, 200));
	p.drawRect(boxRect);

	return img;
}

QByteArray PieceNumRecognizer::preparePayload(const QImage& img) const {
	QByteArray ba;
	QBuffer buffer(&ba);
	buffer.open(QIODevice::WriteOnly);
	img.save(&buffer, "PNG");

	QJsonObject json;
	json["model"] = ConfigSettings::get<VarSetting<QString >> ("ollamaModel")->getValue();
	json["prompt"] = ConfigSettings::get<VarSetting<QString >> ("ollamaPrompt")->getValue();
	json["stream"] = false;

	QJsonArray images;
	images.append(QString::fromLatin1(ba.toBase64()));
	json["images"] = images;

	return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

QPair<QString, QByteArray> PieceNumRecognizer::sendRequest(const QByteArray& payload) const {
	QNetworkRequest request(QUrl("http://localhost:11434/api/generate"));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	QNetworkAccessManager manager;
	QNetworkReply *reply = manager.post(request, payload);

	QEventLoop loop;
	QTimer timer;

	timer.setSingleShot(true);
	timer.start(30000);

	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

	loop.exec();
	reply->deleteLater();

	if (!timer.isActive()) {
		reply->abort();
		return {"Error: HTTP request timeout", QByteArray{}};
	}

	timer.stop();

	if (reply->error() != QNetworkReply::NoError) {
		return {QString("HTTP error: %1").arg(reply->errorString()), QByteArray{}};
	}

	QByteArray raw = reply->readAll();
	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
	if (err.error != QJsonParseError::NoError) {
		return {QString("JSON parse error: %1").arg(err.errorString()), raw};
	} else if (!doc.isObject()) {
		return {"Error: received not a JSON object", raw};
	}

	QString response = doc.object().value("response").toString();
	QRegularExpression re(ConfigSettings::get<VarSetting<QString >> ("ollamaRegex")->getValue());
	auto match = re.match(response);
	if (!match.hasMatch()) {
		return {QString("Error: unexpected response: %1").arg(response), raw};
	}

	return {QString("%1\n%2").arg(match.captured(2)).arg(match.captured(1)), raw};
}
