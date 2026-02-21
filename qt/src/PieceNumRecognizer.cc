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

static QString DEFAULT_OLLAMA_MODEL = "blaifa/InternVL3_5:8b";

static QString DEFAULT_OLLAMA_PROMPT = R"(You are extracting structured data from a part of CaDA "bill of materials" page.

All words here are independent and arranged vertically in columns.

Under magenta box there are two words arranged in a column:
1. Piece quantity, which appears in one of these exact formats: "<number>x" or "x<number>" (examples: "14x", "2x", "x67", "x1").
2. Piece SKU, which contains 8 or more alphanumeric characters.

Extract ONLY these two words.

Important:
- The image may contain other SKUs, quantities, or text elsewhere. Ignore all of them.

Output rules:
- Use exactly this JSON structure: {"<piece_SKU>":"<piece_quantity>"}
- Do not add explanations, reasoning, comments, or any text outside the JSON structure.
- Do not infer or guess missing information. If either value is unreadable, output an empty string for that field.)";

static QString DEFAULT_OLLAMA_REGEX = R"_("(\S+)"\s*:\s*"(\S+)")_";

static QString DEFAULT_VALIDATE_REGEX = R"_(^(x\d+|\d+x)\n((\d{3}R)?J[A-Z]\d{4}(\d{4}|\.\d{2})?|204RC\d{9})$)_";

static VarSetting<bool>& cfgB(const char* key) {
	return *ConfigSettings::get<VarSetting<bool>>(key);
}

static VarSetting<QString>& cfgS(const char* key) {
	return *ConfigSettings::get<VarSetting<QString>>(key);
}

class DraggableTextEdit : public QPlainTextEdit {
public:
	DraggableTextEdit(const QString& text, StickyTooltip* parent)
		: QPlainTextEdit(text, parent), m_parent(parent) {
		viewport()->setCursor(Qt::OpenHandCursor);
	}

private:
	StickyTooltip* m_parent;

	void mousePressEvent(QMouseEvent* e) override {
		m_parent->mousePressEvent(e);
		if (e->isAccepted()) {
			viewport()->setCursor(Qt::ClosedHandCursor);
		} else {
			QPlainTextEdit::mousePressEvent(e);
		}
	}

	void mouseMoveEvent(QMouseEvent* e) override {
		m_parent->mouseMoveEvent(e);
		if (!e->isAccepted()) {
			QPlainTextEdit::mouseMoveEvent(e);
		}
	}

	void mouseReleaseEvent(QMouseEvent* e) override {
		m_parent->mouseReleaseEvent(e);
		if (e->isAccepted()) {
			viewport()->setCursor(Qt::OpenHandCursor);
		} else {
			QPlainTextEdit::mouseReleaseEvent(e);
		}
	}
};

StickyTooltip::StickyTooltip(const QString& text)
	: QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_prevText(text) {
	setAttribute(Qt::WA_DeleteOnClose);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents, false);

	setCursor(Qt::OpenHandCursor);

	m_edit = new DraggableTextEdit(text, this);
	m_edit->document()->setDocumentMargin(2.0);
	connect(m_edit->document(), &QTextDocument::contentsChanged, this, &StickyTooltip::onTextChanged);

	QFontMetrics fm(m_edit->font());
	int maxH = fm.lineSpacing() * 2 + 4 * 2;
	QString placeHolder(std::max(text.split("\n").last().size(), (qsizetype)8), QChar('X'));
	int maxW = fm.boundingRect(placeHolder).width() * 4 / 3 + 3 * 2;
	m_edit->setFixedSize(maxW, maxH);
	m_edit->setFrameStyle(QFrame::Box);
	m_edit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	adjustSize();

	m_closeTimer = new QTimer(this);
	m_closeTimer->setSingleShot(true);
	m_closeTimer->start(10000);
	connect(m_closeTimer, &QTimer::timeout, this, &QWidget::close);

	connect(qApp, &QApplication::focusChanged, this, &StickyTooltip::onFocusChanged);
	connect(&cfgS("pnrregex"), &VarSetting<QString>::changed, this, &StickyTooltip::validateContent);
	validateContent();
}

void StickyTooltip::onTextChanged() {
	QString newText = m_edit->toPlainText();
	if (newText != m_prevText) {
		MAIN->getOutputEditor()->modifyTail(m_prevText + "\n", newText + "\n");
		m_prevText = newText;
	}
	validateContent();
}

void StickyTooltip::onFocusChanged(QWidget *old, QWidget *now) {
	if (now == this || isAncestorOf(now)) {
		m_closeTimer->stop();
	} else if (old == this || isAncestorOf(old)) {
		m_closeTimer->start(10000);
	}
}

void StickyTooltip::mousePressEvent(QMouseEvent* e) {
	if (e->button() == Qt::LeftButton) {
		m_dragging = true;
		m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
		setCursor(Qt::ClosedHandCursor);
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
		setCursor(Qt::OpenHandCursor);
		e->accept();
	}
}

void StickyTooltip::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		close();
	} else {
		QWidget::keyPressEvent(e);
	}
}

void StickyTooltip::validateContent() {
	QString text = m_edit->toPlainText();
	bool isValid = QRegularExpression(cfgS("pnrregex").getValue()).match(text).hasMatch();
	QPalette p = m_edit->palette();
	p.setColor(QPalette::Base, isValid ? QColor("#cfc") : QColor("#fcc"));
	m_edit->setPalette(p);
}

PieceNumRecognizer::PieceNumRecognizer(DisplayerToolSelect* tool)
	: QObject(tool), m_tool(tool) {
	ADD_SETTING(VarSetting<QString> ("ollamaapi", DEFAULT_OLLAMA_API));
	ADD_SETTING(VarSetting<QString> ("ollamamodel", DEFAULT_OLLAMA_MODEL));
	ADD_SETTING(VarSetting<QString> ("ollamaprompt", DEFAULT_OLLAMA_PROMPT));
	ADD_SETTING(VarSetting<QString> ("ollamaregex", DEFAULT_OLLAMA_REGEX));
	ADD_SETTING(VarSetting<QString> ("pnrregex", DEFAULT_VALIDATE_REGEX));
	ADD_SETTING(VarSetting<bool> ("ollamadebug", false));
}

PieceNumRecognizer::~PieceNumRecognizer() {
}

void PieceNumRecognizer::recognizePieceNum(std::variant<NumberedDisplayerSelection*, QPointF> source) {
	if (m_avgPieceNumSize.isEmpty()) {
		QMessageBox::warning(MAIN, _("Recognition errors"), _("You must set average piece num size first."));
		return;
	}

	MAIN->setOutputPaneVisible(true);

	QImage img = prepareImage(source);
	QJsonObject json = prepareOcrPayload(img);

	QString debugBasePath;
	if (cfgB("ollamadebug").getValue()) {
		debugBasePath = QDir(QDir::tempPath()).filePath("gImageReaderLastOllama");
		img.save(debugBasePath + "Request.png");

		QFile file(debugBasePath + "Request.json");
		if (file.open(QIODevice::WriteOnly)) {
			file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
			file.close();
		}
	}

	auto [response, isError] = sendOcrRequest(json, debugBasePath.isEmpty() ? "" : (debugBasePath + "Response.bin"));
	if (isError) {
		QMessageBox::warning(MAIN, _("Error"), response);
	} else {
		MAIN->getOutputEditor()->appendText(response + "\n");

		// QToolTip hides immediately, maybe because view loses keyboard focus
		// when appending text to editor
		auto tooltip = new StickyTooltip(response);
		QPointF posF;
		if (auto sel = std::get_if<NumberedDisplayerSelection*>(&source)) {
			posF = (*sel)->rect().bottomLeft();
		} else {
			posF = std::get<QPointF>(source);
		}
		QPoint pos = m_tool->getDisplayer()->mapToGlobal(m_tool->getDisplayer()->mapFromScene(posF));
		pos.ry() -= tooltip->height();
		tooltip->move(pos);
		tooltip->show();
	}

	if (!debugBasePath.isEmpty()) {
		MAIN->showStatus(_("Saved Ollama communication as %1Re*.*").arg(debugBasePath));
	}
}

QImage PieceNumRecognizer::prepareImage(std::variant<NumberedDisplayerSelection*, QPointF> source) const {
	QRectF pieceRect;
	if (auto sel = std::get_if<NumberedDisplayerSelection*>(&source)) {
		pieceRect = (*sel)->rect();
	} else {
		auto pos = std::get<QPointF>(source);
		pos.ry() -= m_avgPieceNumSize.height();
		pieceRect = QRectF(pos, m_avgPieceNumSize);
	}
	pieceRect.setTop(pieceRect.top() + pieceRect.height() / 2.0);

	QPointF offset(m_avgPieceNumSize.width() / 2.0, 1.0);
	QRectF exportRect = pieceRect;
	exportRect.adjust(-offset.x(), -offset.y(), m_avgPieceNumSize.width() * 1.5 - exportRect.width(), 1.5 * m_avgPieceNumSize.height());
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

QJsonObject PieceNumRecognizer::prepareOcrPayload(const QImage& img) const {
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

	return json;
}

QPair<QJsonObject, QString> PieceNumRecognizer::sendRequest(const QUrl& endpoint, int timeoutMs, const QJsonObject* payload, const QString* debugPath) {
	QNetworkRequest request(endpoint);
	QNetworkAccessManager manager;
	QNetworkReply *reply;
	if (payload) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
		reply = manager.post(request, QJsonDocument(*payload).toJson(QJsonDocument::Compact));
	} else {
		reply = manager.get(request);
	}

	QEventLoop loop;
	QTimer timer;

	timer.setSingleShot(true);
	timer.start(timeoutMs);

	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

	loop.exec();
	reply->deleteLater();

	if (!timer.isActive()) {
		reply->abort();
		return {QJsonObject{}, "Error: HTTP request timeout"};
	}

	timer.stop();

	if (reply->error() != QNetworkReply::NoError) {
		return {QJsonObject{}, QString("HTTP error: %1").arg(reply->errorString())};
	}

	QByteArray raw = reply->readAll();
	if (debugPath) {
		QFile file(*debugPath);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(raw);
			file.close();
		}
	}

	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
	if (err.error != QJsonParseError::NoError) {
		return {QJsonObject{}, QString("JSON parse error: %1").arg(err.errorString())};
	} else if (!doc.isObject()) {
		return {QJsonObject{}, "Error: received not a JSON object"};
	}

	return {doc.object(), QString{}};
}

QPair<QString, bool> PieceNumRecognizer::sendOcrRequest(const QJsonObject& payload, const QString& debugPath) const {
	QUrl endpoint(cfgS("ollamaapi").getValue());
	endpoint.setPath("/api/generate");

	auto res = sendRequest(endpoint, 30000, &payload, &debugPath);
	if (!res.second.isEmpty()) {
		return {res.second, true};
	}

	QString response = res.first.value("response").toString();
	auto match = QRegularExpression(cfgS("ollamaregex").getValue()).match(response);
	if (!match.hasMatch()) {
		return {QString("Error: unexpected response: %1").arg(response), true};
	}

	return {QString("%1\n%2").arg(match.captured(2)).arg(match.captured(1)), false};
}

void PieceNumRecognizer::showConfig() {
	QDialog dlg;
	dlg.setWindowTitle(_("Piece Num Recognizer Configuration"));
	dlg.setWindowIcon(QIcon::fromTheme("preferences-system"));

	auto apiEdit = new QLineEdit(cfgS("ollamaapi").getValue(), &dlg);

	auto modelsCombo = new QComboBox(&dlg); 
	modelsCombo->setEditable(true);
	modelsCombo->setEditText(cfgS("ollamamodel").getValue());

	auto fetchBtn = new QToolButton(&dlg);
	fetchBtn->setIcon(QIcon::fromTheme("view-refresh"));
	fetchBtn->setToolTip(_("Fetch models"));
	connect(fetchBtn, &QToolButton::clicked, [&]() {
		fetchBtn->setEnabled(false);

		QUrl endpoint(apiEdit->text());
		endpoint.setPath("/api/tags");

		auto [json, error] = sendRequest(endpoint, 10000);
		if (!error.isEmpty()) {
			QMessageBox::warning(&dlg, _("Error"), error);
		} else {
			modelsCombo->clear();
			auto models = json.value("models").toArray();
			QStringList modelNames;
			for (const auto& model : models) {
				modelNames << model.toObject().value("name").toString();
			}
			modelNames.removeAll("");
			modelNames.sort();
			for (const auto& name : modelNames) {
				modelsCombo->addItem(name);
			}
			modelsCombo->showPopup();
		}

		fetchBtn->setEnabled(true);
	});

	auto modelLayout = new QHBoxLayout;
	modelLayout->setContentsMargins(0, 0, 0, 0);
	modelLayout->setSpacing(0);
	modelLayout->addWidget(modelsCombo);
	modelLayout->addWidget(fetchBtn);

	auto promptEdit = new QPlainTextEdit(cfgS("ollamaprompt").getValue(), &dlg);
	auto responseRegexEdit = new QLineEdit(cfgS("ollamaregex").getValue(), &dlg);
	auto validateRegexEdit = new QLineEdit(cfgS("pnrregex").getValue(), &dlg);
	auto debugCheck = new QCheckBox(_("Save last request data as <TempDir>/gImageReaderLastOllamaRe*.*"), &dlg);
	debugCheck->setChecked(cfgB("ollamadebug").getValue());

	auto form = new QFormLayout;
	form->addRow(_("Ollama API:"), apiEdit);
	form->addRow(_("Model:"), modelLayout);
	form->addRow(_("Prompt:"), promptEdit);
	form->addRow(_("Response regex:"), responseRegexEdit);
	form->addRow(_("Validate regex:"), validateRegexEdit);
	form->addRow(debugCheck);

	auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
	buttons->addButton(QDialogButtonBox::RestoreDefaults);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto mainLayout = new QVBoxLayout;
	mainLayout->addLayout(form);
	mainLayout->addSpacing(8);
	mainLayout->addWidget(buttons);

	dlg.setLayout(mainLayout);
	dlg.setMinimumSize(420, 300);
	dlg.resize(532, 390);

	auto onChanged = [&](auto&&...) {
		bool canSave = apiEdit->text() != cfgS("ollamaapi").getValue() ||
			modelsCombo->currentText() != cfgS("ollamamodel").getValue() ||
			promptEdit->toPlainText() != cfgS("ollamaprompt").getValue() ||
			responseRegexEdit->text() != cfgS("ollamaregex").getValue() ||
			validateRegexEdit->text() != cfgS("pnrregex").getValue() ||
			debugCheck->isChecked() != cfgB("ollamadebug").getValue();

		bool canRestore = apiEdit->text() != cfgS("ollamaapi").getDefaultValue() ||
			modelsCombo->currentText() != cfgS("ollamamodel").getDefaultValue() ||
			promptEdit->toPlainText() != cfgS("ollamaprompt").getDefaultValue() ||
			responseRegexEdit->text() != cfgS("ollamaregex").getDefaultValue() ||
			validateRegexEdit->text() != cfgS("pnrregex").getDefaultValue() ||
			debugCheck->isChecked() != cfgB("ollamadebug").getDefaultValue();

		buttons->button(QDialogButtonBox::Save)->setEnabled(canSave);
		buttons->button(QDialogButtonBox::RestoreDefaults)->setEnabled(canRestore);
	};
	onChanged();

	connect(apiEdit, &QLineEdit::textChanged, onChanged);
	connect(modelsCombo, &QComboBox::editTextChanged, onChanged);
	connect(promptEdit, &QPlainTextEdit::textChanged, onChanged);
	connect(responseRegexEdit, &QLineEdit::textChanged, onChanged);
	connect(validateRegexEdit, &QLineEdit::textChanged, onChanged);
	connect(debugCheck, &QCheckBox::toggled, onChanged);

	connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, [&]() {
		apiEdit->setText(cfgS("ollamaapi").getDefaultValue());
		modelsCombo->setEditText(cfgS("ollamamodel").getDefaultValue());
		promptEdit->setPlainText(cfgS("ollamaprompt").getDefaultValue());
		responseRegexEdit->setText(cfgS("ollamaregex").getDefaultValue());
		validateRegexEdit->setText(cfgS("pnrregex").getDefaultValue());
		debugCheck->setChecked(cfgB("ollamadebug").getDefaultValue());
	});

	if (QDialog::Accepted == dlg.exec()) {
		cfgS("ollamaapi").setValue(apiEdit->text());
		cfgS("ollamamodel").setValue(modelsCombo->currentText());
		cfgS("ollamaprompt").setValue(promptEdit->toPlainText());
		cfgS("ollamaregex").setValue(responseRegexEdit->text());
		cfgS("pnrregex").setValue(validateRegexEdit->text());
		cfgB("ollamadebug").setValue(debugCheck->isChecked());
	}
}
