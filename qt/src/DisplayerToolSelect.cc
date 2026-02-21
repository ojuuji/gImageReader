/* -*- Mode: C++; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*-  */
/*
 * DisplayerToolSelect.cc
 * Copyright (C) 2013-2025 Sandro Mani <manisandro@gmail.com>
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

#include "DisplayerToolSelect.hh"
#include "Displayer.hh"
#include "FileDialogs.hh"
#include "MainWindow.hh"
#include "Recognizer.hh"
#include "Utils.hh"

#include <cmath>
#define USE_STD_NAMESPACE
#include <tesseract/baseapi.h>
#undef USE_STD_NAMESPACE
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QStyle>


static QImage addMargin(const QImage& src, const QSize& margin, const QColor& bgColor) {
	int newWidth  = src.width()  + 2 * margin.width();
	int newHeight = src.height() + 2 * margin.height();

	QImage result(newWidth, newHeight, src.format());
	result.fill(bgColor);

	QPainter painter(&result);
	painter.drawImage(margin.width(), margin.height(), src);
	painter.end();

	return result;
}

static QSet<QPoint> fillHoles(const QSet<QPoint>& mask, const QRect& bbox) {
	QSet<QPoint> filled = mask;
	QSet<QPoint> visitedZeros;

	for (int x = bbox.left(); x < bbox.right(); x++) {
		for (int y = bbox.top(); y < bbox.bottom(); y++) {
			QPoint p(x, y);
			if (!mask.contains(p) && !visitedZeros.contains(p)) {
				QSet<QPoint> region;
				bool touchesBorder = false;

				QQueue<QPoint> queue;
				queue.enqueue(p);

				while (!queue.isEmpty()) {
					QPoint q = queue.dequeue();
					if (mask.contains(q) || visitedZeros.contains(q)) {
						continue;
					}

					visitedZeros.insert(q);
					region.insert(q);

					if (q.x() == bbox.left() || q.x() == bbox.right() - 1 || q.y() == bbox.top() || q.y() == bbox.bottom() - 1) {
						touchesBorder = true;
					}

					// Add neighbors (4-connectivity)
					if (q.x() > bbox.left()) {
						queue.enqueue(QPoint(q.x() - 1, q.y()));
					}
					if (q.x() < bbox.right() - 1) {
						queue.enqueue(QPoint(q.x() + 1, q.y()));
					}
					if (q.y() > bbox.top()) {
						queue.enqueue(QPoint(q.x(), q.y() - 1));
					}
					if (q.y() < bbox.bottom() - 1) {
						queue.enqueue(QPoint(q.x(), q.y() + 1));
					}
				}

				if (!touchesBorder) {
					filled.unite(region);
				}
			}
		}
	}

	return filled;
}

static QPair<QSet<QPoint>, QRect> calcMask(const QImage& img, const QPoint& start, const QColor& bgColor, const int bgColorDiff) {
	QSet<QPoint> visited;
	QSet<QPoint> mask;
	QQueue<QPoint> queue;
	queue.enqueue(start);

	int minX = start.x();
	int maxX = start.x();
	int minY = start.y();
	int maxY = start.y();

	while (!queue.isEmpty()) {
		QPoint p = queue.dequeue();
		if (visited.contains(p)) {
			continue;
		}
		visited.insert(p);

		QColor c = img.pixelColor(p);
		int diffR = qAbs(c.red() - bgColor.red());
		int diffG = qAbs(c.green() - bgColor.green());
		int diffB = qAbs(c.blue() - bgColor.blue());
		if (diffR <= bgColorDiff && diffG <= bgColorDiff && diffB <= bgColorDiff) {
			continue; // stop at background
		}
		mask.insert(p);

		// Update bounding box
		minX = qMin(minX, p.x());
		maxX = qMax(maxX, p.x());
		minY = qMin(minY, p.y());
		maxY = qMax(maxY, p.y());

		// Add neighbors (4-connectivity)
		if (p.x() > 0) {
			queue.enqueue(QPoint(p.x() - 1, p.y()));
		}
		if (p.x() < img.width() - 1) {
			queue.enqueue(QPoint(p.x() + 1, p.y()));
		}
		if (p.y() > 0) {
			queue.enqueue(QPoint(p.x(), p.y() - 1));
		}
		if (p.y() < img.height() - 1) {
			queue.enqueue(QPoint(p.x(), p.y() + 1));
		}
	}

	QRect bbox(QPoint(minX, minY), QPoint(maxX, maxY));
	QSet<QPoint> maskFilled = fillHoles(mask, bbox);

	return {maskFilled, bbox};
}

DisplayerToolSelect::DisplayerToolSelect(Displayer* displayer, QObject* parent)
	: DisplayerTool(displayer, parent), m_pnr(this) {
	displayer->setCursor(Qt::CrossCursor);
	updateRecognitionModeLabel();
}

DisplayerToolSelect::~DisplayerToolSelect() {
	clearSelections();
}

void DisplayerToolSelect::contextMenuEvent(QContextMenuEvent* event) {
	QMenu menu;
	QAction* clearAction = new QAction(QIcon::fromTheme("edit-delete"), _("Clear selection"), &menu);
	clearAction->setEnabled(!m_selections.isEmpty());
	QAction* saveSelectionsAction = new QAction(QIcon::fromTheme("document-save-as"), _("Save selections as images"), &menu);
	QAction* savePageAction = new QAction(QIcon::fromTheme("document-save-as"), _("Save page as image"), &menu);
	QAction* setBgColorAction = new QAction(_("Set piece image background color") + "\tCtrl+Alt+LeftClick", &menu);
	QAction* incBgColorDiffAction = new QAction(_("Increase image background color diff") + "\tAlt+WheelUp", &menu);
	QAction* decBgColorDiffAction = new QAction(_("Decrease image background color diff") + "\tAlt+WheelDown", &menu);
	QAction* recognizeImgAction = new QAction(_("Recognize piece image") + "\tAlt+LeftClick", &menu);
	QAction* recognizeImgNumAction = new QAction(_("Recognize piece image and num") + "\tAlt+Shift+LeftClick", &menu);
	QAction* recognizeNumAction = new QAction(_("Recognize piece num") + "\tCtrl+Alt+Shift+LeftClick", &menu);
	QAction* showPnrConfigAction = new QAction(QIcon::fromTheme("preferences-system"), _("Configure piece num recognizer"), &menu);
	menu.addActions(QList<QAction*>() << clearAction << saveSelectionsAction << savePageAction
		<< setBgColorAction << incBgColorDiffAction << decBgColorDiffAction << recognizeImgAction
		<< recognizeImgNumAction << recognizeNumAction << showPnrConfigAction);
	QAction* selected = menu.exec(event->globalPos());
	if (selected == clearAction) {
		clearSelections();
	} else if (selected == saveSelectionsAction) {
		saveAllSelections();
	} else if (selected == savePageAction) {
		saveSelection();
	} else if (selected == setBgColorAction) {
		setPieceImgBgColor(event->pos());
	} else if (selected == incBgColorDiffAction || selected == decBgColorDiffAction) {
		modifyBgColorDiff(selected == incBgColorDiffAction);
	} else if (selected == recognizeImgAction || selected == recognizeImgNumAction) {
		recognizePiece(event->pos(), selected == recognizeImgNumAction);
	} else if (selected == recognizeNumAction) {
		m_pnr.recognizePieceNum(m_displayer->mapToSceneClamped(event->pos()));
	} else if (selected == showPnrConfigAction) {
		m_pnr.showConfig();
	}
}

QPair<QRectF, PostProcessor> DisplayerToolSelect::calcBoundingBox(const QPoint& pos) {
	QPoint start = (m_displayer->mapToSceneClamped(pos) - m_displayer->getSceneBoundingRect().topLeft()).toPoint();
	QImage img = m_displayer->getImage(m_displayer->getSceneBoundingRect());
	if (!img.rect().contains(start)) {
		return {};
	}

	QColor bgColor = m_bgColor;
	if (bgColor == QColor()) {
		const int bgColorOffsetDiv = 40;
		bgColor = img.pixelColor(img.rect().width() / bgColorOffsetDiv, img.rect().height() / bgColorOffsetDiv);
	}

	auto [mask, maskRect] = calcMask(img, start, bgColor, m_bgColorDiff);
	QRectF boundingRect = maskRect.toRectF().translated(m_displayer->getSceneBoundingRect().topLeft());

	return QPair<QRectF, PostProcessor>(
		boundingRect,
		[mask, bgColor, displayer = m_displayer](QImage& img, const QRectF& selectionRect) {
			QPoint topLeft = (selectionRect.topLeft() - displayer->getSceneBoundingRect().topLeft()).toPoint();

			for (int x = 0; x < img.width(); x++) {
				for (int y = 0; y < img.height(); y++) {
					QPoint p(x, y);
					if (!mask.contains(p + topLeft)) {
						img.setPixelColor(p, bgColor);
					}
				}
			}

			QString message;
			for (const QPoint& p : mask) {
				if (p.x() < topLeft.x() || p.x() >= topLeft.x() + img.width() || p.y() < topLeft.y() || p.y() >= topLeft.y() + img.height()) {
					message = _("Mask point %1x%2 is outside selection [%3x%4, %5x%6).").arg(p.x()).arg(p.y())
						.arg(topLeft.x()).arg(topLeft.y()).arg(topLeft.x() + img.width()).arg(topLeft.y() + img.height());
				}
			}

			QSize margin(qMax(20, qMin(50, img.width() / 2)), qMax(20, qMin(50, img.height() / 2)));
			img = addMargin(img, margin, bgColor);

			return message;
		}
	);
}

void DisplayerToolSelect::mousePressEvent(QMouseEvent* event) {
	if (event->button() == Qt::LeftButton && m_curSel == nullptr) {
		if ((event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::AltModifier) && (event->modifiers() & Qt::ShiftModifier)) {
			m_pnr.recognizePieceNum(m_displayer->mapToSceneClamped(event->pos()));
		} else if ((event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::AltModifier)) {
			setPieceImgBgColor(event->pos());
		} else if (event->modifiers() & Qt::ControlModifier) {
			m_curSel = new NumberedDisplayerSelection(this, 1 + m_selections.size(), m_displayer->mapToSceneClamped(event->pos()));
			m_curSel->setZValue(1 + m_selections.size());
			m_displayer->scene()->addItem(m_curSel);
			event->accept();
		} else if (event->modifiers() & Qt::AltModifier) {
			recognizePiece(event->pos(), event->modifiers() & Qt::ShiftModifier);
			event->accept();
		}
	}
}

void DisplayerToolSelect::mouseMoveEvent(QMouseEvent* event) {
	if (m_curSel) {
		QPointF p = m_displayer->mapToSceneClamped(event->pos());
		m_curSel->setPoint(p);
		m_displayer->ensureVisible(QRectF(p, p));
		event->accept();
	}
}

void DisplayerToolSelect::mouseReleaseEvent(QMouseEvent* event) {
	if (m_curSel) {
		if (m_curSel->rect().width() < 5.0 || m_curSel->rect().height() < 5.0) {
			delete m_curSel;
		} else {
			m_selections.append(m_curSel);
			updateRecognitionModeLabel();
		}
		m_curSel = nullptr;
		event->accept();
	}
}

void DisplayerToolSelect::wheelEvent(QWheelEvent* event) {
	if (event->modifiers() & Qt::AltModifier) {
		modifyBgColorDiff(event->angleDelta().x() > 0);
	}
}

void DisplayerToolSelect::resolutionChanged(double factor) {
	for (NumberedDisplayerSelection* sel : m_selections) {
		sel->scale(factor);
	}
}

void DisplayerToolSelect::rotationChanged(double delta) {
	QTransform t;
	t.rotate(delta);
	for (NumberedDisplayerSelection* sel : m_selections) {
		sel->rotate(t);
	}
}

QList<QImage> DisplayerToolSelect::getOCRAreas() {
	QList<QImage> images;
	if (m_selections.empty()) {
		images.append(m_displayer->getImage(m_displayer->getSceneBoundingRect()));
	} else {
		for (const NumberedDisplayerSelection* sel : m_selections) {
			images.append(m_displayer->getImage(sel->rect()));
		}
	}
	return images;
}

void DisplayerToolSelect::clearSelections() {
	qDeleteAll(m_selections);
	m_selections.clear();
	updateRecognitionModeLabel();
}

void DisplayerToolSelect::removeSelection(int num) {
	delete m_selections[num - 1];
	m_selections.removeAt(num - 1);
	for (int i = 0, n = m_selections.size(); i < n; ++i) {
		m_selections[i]->setNumber(1 + i);
		m_selections[i]->setZValue(1 + i);
	}
}

void DisplayerToolSelect::reorderSelection(int oldNum, int newNum) {
	NumberedDisplayerSelection* sel = m_selections[oldNum - 1];
	m_selections.removeAt(oldNum - 1);
	m_selections.insert(newNum - 1, sel);
	for (int i = 0, n = m_selections.size(); i < n; ++i) {
		m_selections[i]->setNumber(1 + i);
		m_selections[i]->setZValue(1 + i);
	}
}

void DisplayerToolSelect::saveSelection(NumberedDisplayerSelection* selection) {
	QString title = selection ? _("Save Selection Image") : _("Save Page Image");
	QString initialFilename = selection ? _("selection.png") : _("page.png");
	QString filter = QString("%1 (*.png);;%2 (*.jpg)").arg(_("PNG Images")).arg(_("JPG Images"));
	QString filename = FileDialogs::saveDialog(title, initialFilename, "outputdir", filter, true);
	if (!filename.isEmpty()) {
		QRectF rect = selection ? selection->rect() : m_displayer->getSceneBoundingRect();
		QImage img = m_displayer->getImage(rect);
		QString message;
		if (selection && selection->postProcessor()) {
			message = selection->postProcessor()(img, rect);
		}
		img.save(filename);
		if (!message.isEmpty()) {
			QMessageBox::warning(MAIN, _("Recognition errors"), message);
		}
	}
}

void DisplayerToolSelect::saveAllSelections() {
	QString filter = QString("%1 (*.png);;%2 (*.jpg)").arg(_("PNG Images")).arg(_("JPG Images"));
	QString filename = FileDialogs::saveDialog(_("Save Selections Images"), _("selection.png"), "outputdir", filter, false);
	if (!filename.isEmpty()) {
		QFileInfo fi(filename);
		QString baseName = fi.completeBaseName();
		QString ext = fi.suffix();
		int width = QString::number(m_selections.size()).length();
		int index = 0;
		QStringList messages;
		for (NumberedDisplayerSelection* sel : m_selections) {
			QImage img = m_displayer->getImage(sel->rect());
			auto imgFileName = QString("%1-%2.%3").arg(baseName).arg(index, width, 10, QChar('0')).arg(ext);
			auto path = fi.dir().absoluteFilePath(imgFileName);
			if (sel->postProcessor()) {
				QString message = sel->postProcessor()(img, sel->rect());
				if (!message.isEmpty()) {
					messages << _("Selection #%1: %2").arg(index + 1).arg(message);
				}
			}
			img.save(path);
			index++;
		}
		if (!messages.isEmpty()) {
			QMessageBox::warning(MAIN, _("Recognition errors"), messages.join("\n"));
		}
	}
}

void DisplayerToolSelect::updateRecognitionModeLabel() {
	MAIN->getRecognizer()->setRecognizeMode(m_selections.isEmpty() ? _("Recognize all") : _("Recognize selection"));
}

void DisplayerToolSelect::autodetectLayout(bool noDeskew) {
	clearSelections();

	double avgDeskew = 0.0;
	int nDeskew = 0;
	QList<QRectF> rects;
	QImage img = m_displayer->getImage(m_displayer->getSceneBoundingRect());

	// Perform layout analysis
	Utils::busyTask([&nDeskew, &avgDeskew, &rects, &img] {
		QByteArray current = setlocale(LC_ALL, NULL);
		setlocale(LC_ALL, "C");
		tesseract::TessBaseAPI tess;
		tess.InitForAnalysePage();
		setlocale(LC_ALL, current.constData());
		tess.SetPageSegMode(tesseract::PSM_AUTO_ONLY);
		tess.SetImage(img.bits(), img.width(), img.height(), 4, img.bytesPerLine());
		tesseract::PageIterator* it = tess.AnalyseLayout();
		if (it && !it->Empty(tesseract::RIL_BLOCK)) {
			do {
				int x1, y1, x2, y2;
				tesseract::Orientation orient;
				tesseract::WritingDirection wdir;
				tesseract::TextlineOrder tlo;
				float deskew;
				it->BoundingBox(tesseract::RIL_BLOCK, &x1, &y1, &x2, &y2);
				it->Orientation(&orient, &wdir, &tlo, &deskew);
				avgDeskew += deskew;
				++nDeskew;
				float width = x2 - x1, height = y2 - y1;
				float margin = 2;
				if (width > 10 && height > 10) {
					rects.append(QRectF(x1 - 0.5 * img.width() - margin, y1 - 0.5 * img.height() - margin, width + 2 * margin, height + 2 * margin));
				}
			} while (it->Next(tesseract::RIL_BLOCK));
		}
		delete it;
		return true;
	}, _("Performing layout analysis"));

	// If a somewhat large deskew angle is detected, automatically rotate image and redetect layout,
	// unless we already attempted to rotate (to prevent endless loops)
	avgDeskew = qRound(((avgDeskew / nDeskew) / M_PI * 180.0) * 10.0) / 10.0;
	if (std::abs(avgDeskew) > 0.1 && !noDeskew) {
		double newangle = m_displayer->getCurrentAngle() - avgDeskew;
		m_displayer->setup(nullptr, nullptr, &newangle);
		autodetectLayout(true);
	} else {
		// Merge overlapping rectangles
		for (int i = rects.size(); i-- > 1;) {
			for (int j = i; j-- > 0;) {
				if (rects[j].intersects(rects[i])) {
					rects[j] = rects[j].united(rects[i]);
					rects.removeAt(i);
					break;
				}
			}
		}
		for (int i = 0, n = rects.size(); i < n; ++i) {
			m_selections.append(new NumberedDisplayerSelection(this, 1 + i, rects[i].topLeft()));
			m_selections.back()->setPoint(rects[i].bottomRight());
			m_displayer->scene()->addItem(m_selections.back());
		}
		updateRecognitionModeLabel();
	}
}

void DisplayerToolSelect::recognizePiece(QPoint pos, bool includePieceNum) {
	auto [rect, postProcessor] = calcBoundingBox(pos);
	if (rect.width() > 10.0 && rect.height() > 10.0) {
		m_selections.append(new NumberedDisplayerSelection(this, 1 + m_selections.size(), rect.topLeft()));
		m_selections.back()->setPostProcessor(std::move(postProcessor));
		m_selections.back()->setPoint(rect.bottomRight());
		m_displayer->scene()->addItem(m_selections.back());
		updateRecognitionModeLabel();

		if (includePieceNum) {
			m_pnr.recognizePieceNum(m_selections.back());
		}
	}
}

void DisplayerToolSelect::setPieceImgBgColor(QPoint pos) {
	QPointF posF = m_displayer->mapToScene(pos);
	if (m_displayer->getSceneBoundingRect().contains(posF)) {
		QImage img = m_displayer->getImage(QRectF(posF, QSize(1, 1)));
		m_bgColor = img.pixelColor(0, 0);
		MAIN->showStatus(_("Changed background color to %1 (%2, %3, %4).")
			.arg(m_bgColor.name()).arg(m_bgColor.red()).arg(m_bgColor.green()).arg(m_bgColor.blue()));
	} else {
		m_bgColor = QColor();
		MAIN->showStatus(_("Changed background color to auto."));
	}
}

void DisplayerToolSelect::modifyBgColorDiff(bool increase) {
	m_bgColorDiff = qMax(2, m_bgColorDiff + (increase ? 2 : -2));
	MAIN->showStatus(_("Modified background color diff: %1").arg(m_bgColorDiff));
}

///////////////////////////////////////////////////////////////////////////////

void NumberedDisplayerSelection::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
	QMenu menu;

	QWidget* orderWidget = new QWidget(&menu);
	QHBoxLayout* layout = new QHBoxLayout(orderWidget);

	QLabel* orderIcon = new QLabel(&menu);
	int iconSize = orderIcon->style()->pixelMetric(QStyle::PM_SmallIconSize);
	orderIcon->setPixmap(QIcon::fromTheme("object-order-front").pixmap(iconSize, iconSize));
	layout->addWidget(orderIcon);
	layout->setContentsMargins(4, 0, 4, 0);

	QLabel* orderLabel = new QLabel(_("Order:"), &menu);
	orderLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	layout->addWidget(orderLabel);

	QSpinBox* orderSpin = new QSpinBox();
	orderSpin->setRange(1, static_cast<DisplayerToolSelect*> (m_tool)->m_selections.size());
	orderSpin->setValue(m_number);
	orderSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(orderSpin, qOverload<int> (&QSpinBox::valueChanged), this, &NumberedDisplayerSelection::reorderSelection);
	layout->addWidget(orderSpin);

	QWidgetAction* spinAction = new QWidgetAction(&menu);
	spinAction->setDefaultWidget(orderWidget);

	QAction* deleteAction = new QAction(QIcon::fromTheme("edit-delete"), _("Delete") + "\tCtrl+LeftClick", &menu);
	QAction* ocrAction = new QAction(QIcon::fromTheme("insert-text"), _("Recognize"), &menu);
	QAction* ocrClipboardAction = new QAction(QIcon::fromTheme("edit-copy"), _("Recognize to clipboard"), &menu);
	QAction* saveAction = new QAction(QIcon::fromTheme("document-save-as"), _("Save as image"), &menu);
	QAction* setAvgPnSizeAction = new QAction(QIcon::fromTheme("text-plain"), _("Set average piece num size"), &menu);
	menu.addActions(QList<QAction*>() << spinAction << deleteAction << ocrAction << ocrClipboardAction << saveAction << setAvgPnSizeAction);
	QAction* selected = menu.exec(event->screenPos());
	if (selected == deleteAction) {
		static_cast<DisplayerToolSelect*> (m_tool)->removeSelection(m_number);
	} else if (selected == ocrAction) {
		MAIN->getRecognizer()->recognizeImage(m_tool->getDisplayer()->getImage(rect()), Recognizer::OutputDestination::Buffer);
	} else if (selected == ocrClipboardAction) {
		MAIN->getRecognizer()->recognizeImage(m_tool->getDisplayer()->getImage(rect()), Recognizer::OutputDestination::Clipboard);
	} else if (selected == saveAction) {
		static_cast<DisplayerToolSelect*> (m_tool)->saveSelection(this);
	} else if (selected == setAvgPnSizeAction) {
		QSizeF size(rect().size());
		static_cast<DisplayerToolSelect*> (m_tool)->setAvgPieceNumSize(size);
		MAIN->showStatus(_("Updated average piece num size to %1x%2.").arg(size.width()).arg(size.height()));
	}
}

void NumberedDisplayerSelection::mousePressEvent(QGraphicsSceneMouseEvent* event) {
	if (event->modifiers() & Qt::ControlModifier) {
		static_cast<DisplayerToolSelect*> (m_tool)->removeSelection(m_number);
		event->accept();
	}
	else {
		DisplayerSelection::mousePressEvent(event);
	}
}

void NumberedDisplayerSelection::reorderSelection(int newNumber) {
	static_cast<DisplayerToolSelect*> (m_tool)->reorderSelection(m_number, newNumber);
}

void NumberedDisplayerSelection::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
	DisplayerSelection::paint(painter, option, widget);

	painter->setRenderHint(QPainter::Antialiasing, false);
	QRectF r = rect();
	qreal h = 20.0 / m_tool->getDisplayer()->getCurrentScale();
	qreal w = h * (m_number >= 100 ? 1.5 : 1.0);
	if (w > r.width()) {
		h *= (qreal)r.width() / w;
		w = r.width();
	}
	if (h > r.height()) {
		w *= (qreal)r.height() / h;
		h = r.height();
	}
	QRectF box(r.x(), r.y(), w, h);
	painter->setBrush(QPalette().highlight());
	painter->drawRect(box);
	painter->setRenderHint(QPainter::Antialiasing, true);

	if (h > 1.25) {
		QFont font;
		font.setPixelSize(0.8 * h);
		font.setBold(true);
		painter->setFont(font);
		painter->setPen(QPalette().highlightedText().color());
		painter->drawText(box, Qt::AlignCenter, QString::number(m_number));
	}
}
