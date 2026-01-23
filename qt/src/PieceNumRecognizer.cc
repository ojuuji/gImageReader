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
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>

static QString DEFAULT_OLLAMA_API = "http://localhost:11434";

static QString DEFAULT_OLLAMA_MODEL = "llama3.2-vision";

static QString DEFAULT_OLLAMA_PROMPT = R"(You are extracting structured data from a CaDA "bill of materials" page.

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
- Do not infer or guess missing information. If either value is unreadable, output an empty string for that field.)";

static QString DEFAULT_OLLAMA_REGEX = "\"(\\S+)\"\\s*:\\s*\"(\\S+)\"";

static VarSetting<bool>& cfgB(const char* key) {
	return *ConfigSettings::get<VarSetting<bool>>(key);
}

static VarSetting<QString>& cfgS(const char* key) {
	return *ConfigSettings::get<VarSetting<QString>>(key);
}

StickyTooltip::StickyTooltip(const QString& text, QPoint pos)
	: QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_prevText(text) {
	setAttribute(Qt::WA_DeleteOnClose);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents, false);

	m_edit = new QPlainTextEdit(text, this);
	m_edit->document()->setDocumentMargin(2.0);
	connect(m_edit->document(), &QTextDocument::contentsChanged, this, &StickyTooltip::onDocumentContentsChanged);

	QFontMetrics fm(m_edit->font());
	int maxH = fm.lineSpacing() * 2 + verticalPadding();
	int maxW = fm.boundingRect("206RJXNNNNNNNN").width() + 3 * 2;
	m_edit->setFixedSize(maxW, maxH);
	m_edit->setFrameStyle(QFrame::Box);
	m_edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	adjustSize();
	move(pos);
	show();

	m_closeTimer = new QTimer(this);
	m_closeTimer->setSingleShot(true);
	m_closeTimer->start(10000);
	connect(m_closeTimer, &QTimer::timeout, this, &QWidget::close);

	connect(qApp, &QApplication::focusChanged, this, &StickyTooltip::onFocusChanged);
}

void StickyTooltip::onDocumentContentsChanged() {
	QString newText = m_edit->toPlainText();
	if (newText != m_prevText) {
		MAIN->getOutputEditor()->modifyTail(m_prevText + "\n", newText + "\n");
		m_prevText = newText;
	}
}

void StickyTooltip::onFocusChanged(QWidget *old, QWidget *now) {
	if (now && (now == this || this->isAncestorOf(now))) {
		m_closeTimer->stop();
	} else if (old && (old == this || this->isAncestorOf(old))) {
		m_closeTimer->start(10000);
	}
}

int StickyTooltip::verticalPadding() {
	return 8;
}

void StickyTooltip::mousePressEvent(QMouseEvent* e) {
	if (e->button() == Qt::LeftButton) {
		m_dragging = true;
		m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
		e->accept();
	}
}

void StickyTooltip::mouseMoveEvent(QMouseEvent* e) {
	if (m_dragging) {
		move(e->globalPosition().toPoint() - m_dragOffset);
		e->accept();
	}
}

void StickyTooltip::mouseReleaseEvent(QMouseEvent* e) {
	if (e->button() == Qt::LeftButton) {
		m_dragging = false;
		e->accept();
	}
}

void StickyTooltip::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		close();
		return;
	}
	QWidget::keyPressEvent(e);
}

PieceNumRecognizer::PieceNumRecognizer(DisplayerToolSelect* tool)
	: QObject(tool), m_tool(tool) {
	ADD_SETTING(VarSetting<QString> ("ollamaapi", DEFAULT_OLLAMA_API));
	ADD_SETTING(VarSetting<QString> ("ollamamodel", DEFAULT_OLLAMA_MODEL));
	ADD_SETTING(VarSetting<QString> ("ollamaprompt", DEFAULT_OLLAMA_PROMPT));
	ADD_SETTING(VarSetting<QString> ("ollamaregex", DEFAULT_OLLAMA_REGEX));
	ADD_SETTING(VarSetting<bool> ("ollamadebug", false));
}

PieceNumRecognizer::~PieceNumRecognizer() {
}

void PieceNumRecognizer::recognizePieceNum(NumberedDisplayerSelection* sel) {
	if (m_avgPieceNumSize.isEmpty()) {
		QMessageBox::warning(MAIN, _("Recognition errors"), _("You must set average piece num size first."));
		return;
	}

	MAIN->setOutputPaneVisible(true);

	QImage img = prepareImage(sel);
	QByteArray payload = preparePayload(img);

	auto [response, raw] = sendRequest(payload);
	if (!response.isEmpty()) {
		MAIN->getOutputEditor()->appendText(response + "\n");

		qreal minY = sel->rect().bottom() - 2.0 * m_avgPieceNumSize.height() - StickyTooltip::verticalPadding();
		qreal maxY = sel->rect().bottom() - 1.2 * m_avgPieceNumSize.height() - StickyTooltip::verticalPadding();
		QPointF pos(sel->rect().left(), std::clamp(sel->rect().top(), minY, maxY));
		QPoint posGlobal = m_tool->getDisplayer()->mapToGlobal(m_tool->getDisplayer()->mapFromScene(pos));

		// QToolTip hides immediately, maybe because view loses keyboard focus
		// when appending text to editor
		new StickyTooltip(response, posGlobal);
	}

	if (cfgB("ollamadebug").getValue()) {
		QString basePath = QDir(QDir::tempPath()).filePath("gImageReaderLastOllama");
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
	json["model"] = cfgS("ollamamodel").getValue();
	json["prompt"] = cfgS("ollamaprompt").getValue();
	json["stream"] = false;

	QJsonArray images;
	images.append(QString::fromLatin1(ba.toBase64()));
	json["images"] = images;

	return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

QPair<QString, QByteArray> PieceNumRecognizer::sendRequest(const QByteArray& payload) const {
	QUrl endpoint(cfgS("ollamaapi").getValue());
	endpoint.setPath("/api/generate");

	QNetworkRequest request(endpoint);
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
	auto match = QRegularExpression(cfgS("ollamaregex").getValue()).match(response);
	if (!match.hasMatch()) {
		return {QString("Error: unexpected response: %1").arg(response), raw};
	}

	return {QString("%1\n%2").arg(match.captured(2)).arg(match.captured(1)), raw};
}

void PieceNumRecognizer::showConfig() {
	QDialog dlg;
	dlg.setWindowTitle("Piece Num Recognizer Configuration");
	dlg.setWindowIcon(QIcon::fromTheme("preferences-system"));

	auto apiEdit = new QLineEdit(cfgS("ollamaapi").getValue(), &dlg);
	auto modelEdit = new QLineEdit(cfgS("ollamamodel").getValue(), &dlg);
	auto promptEdit = new QPlainTextEdit(cfgS("ollamaprompt").getValue(), &dlg);
	auto regexEdit = new QLineEdit(cfgS("ollamaregex").getValue(), &dlg);

	auto debugLayout = new QVBoxLayout;
	auto debugCheck = new QCheckBox("Enable debug output", &dlg);
	debugCheck->setChecked(cfgB("ollamadebug").getValue());
	debugLayout->addWidget(debugCheck);

	auto debugNote = new QLabel("Save Ollama communication as <tmp_dir>/gImageReaderLastOllamaRe*.*");
	debugNote->setStyleSheet("color: gray; font-size: 11px;");
	debugNote->setContentsMargins(22, 0, 0, 0);
	debugLayout->addWidget(debugNote);

	auto form = new QFormLayout;
	form->addRow("Ollama API:", apiEdit);
	form->addRow("Model name:", modelEdit);
	form->addRow("Prompt:", promptEdit);
	form->addRow("Response regex:", regexEdit);
	form->addRow(debugLayout);

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	buttons->addButton(QDialogButtonBox::RestoreDefaults);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto mainLayout = new QVBoxLayout;
	mainLayout->addLayout(form);
	mainLayout->addWidget(buttons);

	dlg.setLayout(mainLayout);
	dlg.setMinimumSize(420, 300);
	dlg.resize(520, 372);

	auto onChanged = [&](auto&&...) {
		bool canSave = apiEdit->text() != cfgS("ollamaapi").getValue() ||
			modelEdit->text() != cfgS("ollamamodel").getValue() ||
			promptEdit->toPlainText() != cfgS("ollamaprompt").getValue() ||
			regexEdit->text() != cfgS("ollamaregex").getValue() ||
			debugCheck->isChecked() != cfgB("ollamadebug").getValue();

		bool canRestore = apiEdit->text() != cfgS("ollamaapi").getDefaultValue() ||
			modelEdit->text() != cfgS("ollamamodel").getDefaultValue() ||
			promptEdit->toPlainText() != cfgS("ollamaprompt").getDefaultValue() ||
			regexEdit->text() != cfgS("ollamaregex").getDefaultValue() ||
			debugCheck->isChecked() != cfgB("ollamadebug").getDefaultValue();

		buttons->button(QDialogButtonBox::Save)->setEnabled(canSave);
		buttons->button(QDialogButtonBox::RestoreDefaults)->setEnabled(canRestore);
	};
	onChanged();

	connect(apiEdit, &QLineEdit::textChanged, onChanged);
	connect(modelEdit, &QLineEdit::textChanged, onChanged);
	connect(promptEdit, &QPlainTextEdit::textChanged, onChanged);
	connect(regexEdit, &QLineEdit::textChanged, onChanged);
	connect(debugCheck, &QCheckBox::toggled, onChanged);

	connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, [&]() {
		apiEdit->setText(cfgS("ollamaapi").getDefaultValue());
		modelEdit->setText(cfgS("ollamamodel").getDefaultValue());
		promptEdit->setPlainText(cfgS("ollamaprompt").getDefaultValue());
		regexEdit->setText(cfgS("ollamaregex").getDefaultValue());
		debugCheck->setChecked(cfgB("ollamadebug").getDefaultValue());
	});

	if (QDialog::Accepted == dlg.exec()) {
		cfgS("ollamaapi").setValue(apiEdit->text());
		cfgS("ollamamodel").setValue(modelEdit->text());
		cfgS("ollamaprompt").setValue(promptEdit->toPlainText());
		cfgS("ollamaregex").setValue(regexEdit->text());
		cfgB("ollamadebug").setValue(debugCheck->isChecked());
	}
}
