// =============================================================================
//  HexView.cpp
// =============================================================================
#include "HexView.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>

namespace {

// Mistura duas cores (t = 0 → a, t = 1 → b). Usado para derivar cores de
// destaque a partir da paleta, funcionando em tema claro e escuro.
QColor blend(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

}  // namespace

HexView::HexView(QWidget* parent) : QAbstractScrollArea(parent) {
    // Fonte monoespaçada do sistema: todas as colunas ficam alinhadas.
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    setFont(font);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    updateMetrics();
}

// =============================================================================
//  Dados e marcações
// =============================================================================

void HexView::setData(std::vector<uint8_t> data) {
    data_ = std::move(data);
    marks_.assign(data_.size(), 0);
    cursor_ = anchor_ = std::min(cursor_, data_.empty() ? size_t{0} : data_.size() - 1);
    lowNibble_ = false;
    updateMetrics();
    viewport()->update();
    emit cursorMoved(cursorAddress());
}

void HexView::fill(uint32_t start, uint32_t length, uint8_t value) {
    const size_t end = std::min<size_t>(data_.size(), size_t{start} + length);
    for (size_t i = start; i < end; ++i) {
        if (data_[i] != value) {
            data_[i] = value;
            marks_[i] |= Modified;
        }
    }
    viewport()->update();
    emit edited();
}

void HexView::clearMarks() {
    std::fill(marks_.begin(), marks_.end(), 0);
    viewport()->update();
}

void HexView::setDifferences(const std::vector<uint32_t>& addresses) {
    for (auto& m : marks_) m &= ~Difference;
    for (uint32_t a : addresses) {
        if (a < marks_.size()) marks_[a] |= Difference;
    }
    viewport()->update();
}

// =============================================================================
//  Cursor e seleção
// =============================================================================

uint32_t HexView::selectionStart() const { return static_cast<uint32_t>(std::min(cursor_, anchor_)); }
uint32_t HexView::selectionEnd() const { return static_cast<uint32_t>(std::max(cursor_, anchor_)); }

void HexView::goTo(uint32_t address) { moveCursor(address, false); }

void HexView::moveCursor(long long index, bool extend) {
    if (data_.empty()) return;
    index = std::clamp<long long>(index, 0, static_cast<long long>(data_.size()) - 1);
    cursor_ = static_cast<size_t>(index);
    if (!extend) anchor_ = cursor_;
    lowNibble_ = false;
    ensureCursorVisible();
    viewport()->update();
    emit cursorMoved(cursorAddress());
}

void HexView::ensureCursorVisible() {
    const int row = static_cast<int>(cursor_ / kBytesPerRow);
    QScrollBar* bar = verticalScrollBar();
    if (row < bar->value()) {
        bar->setValue(row);
    } else if (row >= bar->value() + visibleRows()) {
        bar->setValue(row - visibleRows() + 1);
    }
}

void HexView::editByte(size_t index, uint8_t value) {
    if (data_[index] != value) {
        data_[index] = value;
        marks_[index] |= Modified;
        emit edited();
    }
}

// =============================================================================
//  Métricas e geometria
// =============================================================================

void HexView::updateMetrics() {
    const QFontMetrics fm(font());
    charWidth_ = fm.horizontalAdvance(QLatin1Char('0'));
    lineHeight_ = fm.height() + 2;  // 2 px de respiro entre linhas
    // Dígitos do endereço: 4 para até 64 KB, mais se necessário.
    addressDigits_ = 4;
    for (size_t limit = 0x10000; data_.size() > limit; limit <<= 4) ++addressDigits_;
    updateScrollBars();
}

int HexView::hexX(int column) const {
    // Endereço + 2 espaços; cada byte ocupa 3 caracteres; espaço extra entre
    // as duas metades de 8 bytes.
    return margin_ + (addressDigits_ + 2 + column * 3 + (column >= 8 ? 1 : 0)) * charWidth_;
}

int HexView::asciiX(int column) const {
    return hexX(kBytesPerRow - 1) + 4 * charWidth_ + column * charWidth_;
}

int HexView::rowCount() const {
    return static_cast<int>((data_.size() + kBytesPerRow - 1) / kBytesPerRow);
}

int HexView::visibleRows() const {
    // A primeira linha da viewport é o cabeçalho fixo.
    return std::max(1, viewport()->height() / lineHeight_ - 1);
}

void HexView::updateScrollBars() {
    QScrollBar* bar = verticalScrollBar();
    bar->setRange(0, std::max(0, rowCount() - visibleRows()));
    bar->setPageStep(visibleRows());
    bar->setSingleStep(1);

    // Rolagem horizontal quando a janela é mais estreita que o conteúdo.
    const int contentWidth = asciiX(kBytesPerRow) + margin_;
    horizontalScrollBar()->setRange(0, std::max(0, contentWidth - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
}

long long HexView::byteAt(const QPoint& pos, bool* inAscii) const {
    const int x = pos.x() + horizontalScrollBar()->value();
    const int row = pos.y() / lineHeight_ - 1 + verticalScrollBar()->value();
    if (pos.y() < lineHeight_ || row < 0) return -1;

    int column = -1;
    *inAscii = false;
    if (x >= asciiX(0)) {
        column = (x - asciiX(0)) / charWidth_;
        *inAscii = true;
    } else {
        // Procura a coluna hex mais próxima do clique.
        for (int c = 0; c < kBytesPerRow; ++c) {
            if (x < hexX(c) + 3 * charWidth_) {
                column = c;
                break;
            }
        }
    }
    if (column < 0 || column >= kBytesPerRow) return -1;
    return static_cast<long long>(row) * kBytesPerRow + column;
}

// =============================================================================
//  Desenho
// =============================================================================

void HexView::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(viewport());
    const QPalette& pal = palette();
    p.fillRect(viewport()->rect(), pal.base());
    p.translate(-horizontalScrollBar()->value(), 0);

    const QFontMetrics fm(font());
    const int ascent = fm.ascent() + 1;

    // Cores derivadas da paleta (funcionam em tema claro e escuro).
    const QColor text = pal.color(QPalette::Text);
    const QColor dim = blend(text, pal.color(QPalette::Base), 0.55);       // bytes 0xFF
    const QColor headerColor = blend(text, pal.color(QPalette::Base), 0.4);
    const QColor modifiedColor = pal.color(QPalette::Link);                // alterados
    const QColor diffBackground = blend(pal.color(QPalette::Base), QColor(220, 50, 47), 0.45);
    const QColor selectionBg = pal.color(QPalette::Highlight);
    const QColor selectionFg = pal.color(QPalette::HighlightedText);
    const QColor cursorBg = blend(pal.color(QPalette::Base), selectionBg, 0.35);

    // --- Cabeçalho fixo com os deslocamentos das colunas -------------------
    p.setPen(headerColor);
    p.drawText(margin_, ascent, QStringLiteral("End."));  // coluna de endereços
    for (int c = 0; c < kBytesPerRow; ++c) {
        p.drawText(hexX(c), ascent, QStringLiteral("%1").arg(c, 2, 16, QLatin1Char('0')).toUpper());
    }
    p.drawText(asciiX(0), ascent, QStringLiteral("ASCII"));
    p.drawLine(0, lineHeight_ - 1, asciiX(kBytesPerRow) + margin_, lineHeight_ - 1);

    if (data_.empty()) return;

    const size_t selStart = selectionStart();
    const size_t selEnd = selectionEnd();
    const bool selecting = hasSelection();

    const int firstRow = verticalScrollBar()->value();
    const int lastRow = std::min(rowCount() - 1, firstRow + visibleRows());

    for (int row = firstRow; row <= lastRow; ++row) {
        const int y = (row - firstRow + 1) * lineHeight_;
        const size_t rowStart = static_cast<size_t>(row) * kBytesPerRow;

        // Endereço da linha.
        p.setPen(headerColor);
        p.drawText(margin_, y + ascent,
                   QStringLiteral("%1").arg(rowStart, addressDigits_, 16, QLatin1Char('0')).toUpper());

        for (int c = 0; c < kBytesPerRow; ++c) {
            const size_t i = rowStart + c;
            if (i >= data_.size()) break;
            const uint8_t value = data_[i];
            const uint8_t mark = marks_[i];

            // Fundo: seleção > diferença > cursor.
            const bool selected = selecting && i >= selStart && i <= selEnd;
            const QRect hexRect(hexX(c) - charWidth_ / 2, y, 3 * charWidth_, lineHeight_);
            const QRect asciiRect(asciiX(c), y, charWidth_, lineHeight_);
            QColor bg;
            if (selected) bg = selectionBg;
            else if (mark & Difference) bg = diffBackground;
            else if (i == cursor_) bg = cursorBg;
            if (bg.isValid()) {
                p.fillRect(hexRect, bg);
                p.fillRect(asciiRect, bg);
            }

            // Cor do texto: alterado > vazio (0xFF) > normal.
            QColor fg = text;
            if (selected) fg = selectionFg;
            else if (mark & Modified) fg = modifiedColor;
            else if (value == 0xFF) fg = dim;
            p.setPen(fg);

            p.drawText(hexX(c), y + ascent,
                       QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
            const QChar ch = (value >= 0x20 && value < 0x7F) ? QChar(value) : QChar('.');
            p.drawText(asciiX(c), y + ascent, QString(ch));
        }
    }

    // Cursor de edição: sublinha o dígito (hex) ou o caractere (ASCII) ativo.
    const int cursorRow = static_cast<int>(cursor_ / kBytesPerRow);
    if (hasFocus() && cursorRow >= firstRow && cursorRow <= lastRow) {
        const int column = static_cast<int>(cursor_ % kBytesPerRow);
        const int y = (cursorRow - firstRow + 2) * lineHeight_ - 2;
        const int x = asciiFocus_ ? asciiX(column) : hexX(column) + (lowNibble_ ? charWidth_ : 0);
        p.fillRect(QRect(x, y, charWidth_, 2), text);
    }
}

// =============================================================================
//  Eventos
// =============================================================================

void HexView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void HexView::changeEvent(QEvent* event) {
    // Fonte ou paleta mudaram (ex.: tema do sistema): recalcula e redesenha.
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange) {
        updateMetrics();
        viewport()->update();
    }
    QAbstractScrollArea::changeEvent(event);
}

void HexView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    bool inAscii = false;
    const long long index = byteAt(event->position().toPoint(), &inAscii);
    if (index < 0) return;
    asciiFocus_ = inAscii;
    moveCursor(index, event->modifiers() & Qt::ShiftModifier);
}

void HexView::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton)) return;
    bool inAscii = false;
    const long long index = byteAt(event->position().toPoint(), &inAscii);
    if (index >= 0) moveCursor(index, true);  // arrastar estende a seleção
}

void HexView::keyPressEvent(QKeyEvent* event) {
    if (data_.empty()) return;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const long long pos = static_cast<long long>(cursor_);
    const long long page = static_cast<long long>(visibleRows()) * kBytesPerRow;

    // --- Navegação ---------------------------------------------------------
    switch (event->key()) {
        case Qt::Key_Left:     moveCursor(pos - 1, shift); return;
        case Qt::Key_Right:    moveCursor(pos + 1, shift); return;
        case Qt::Key_Up:       moveCursor(pos - kBytesPerRow, shift); return;
        case Qt::Key_Down:     moveCursor(pos + kBytesPerRow, shift); return;
        case Qt::Key_PageUp:   moveCursor(pos - page, shift); return;
        case Qt::Key_PageDown: moveCursor(pos + page, shift); return;
        case Qt::Key_Home:
            moveCursor(ctrl ? 0 : pos - pos % kBytesPerRow, shift);
            return;
        case Qt::Key_End:
            moveCursor(ctrl ? static_cast<long long>(data_.size()) - 1
                            : pos - pos % kBytesPerRow + kBytesPerRow - 1,
                       shift);
            return;
        case Qt::Key_Tab:
            // Alterna entre a coluna hexadecimal e a ASCII.
            asciiFocus_ = !asciiFocus_;
            lowNibble_ = false;
            viewport()->update();
            return;
        default:
            break;
    }

    // --- Copiar ------------------------------------------------------------
    if (event->matches(QKeySequence::Copy)) {
        QStringList parts;
        for (uint32_t i = selectionStart(); i <= selectionEnd(); ++i) {
            parts << QStringLiteral("%1").arg(data_[i], 2, 16, QLatin1Char('0')).toUpper();
        }
        QApplication::clipboard()->setText(parts.join(QLatin1Char(' ')));
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        anchor_ = 0;
        cursor_ = data_.size() - 1;
        viewport()->update();
        emit cursorMoved(cursorAddress());
        return;
    }

    // --- Edição ------------------------------------------------------------
    const QString text = event->text();
    if (text.size() != 1 || ctrl) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const QChar ch = text.at(0);

    if (asciiFocus_) {
        // Coluna ASCII: qualquer caractere imprimível grava o byte e avança.
        if (ch.unicode() >= 0x20 && ch.unicode() < 0x7F) {
            editByte(cursor_, static_cast<uint8_t>(ch.unicode()));
            moveCursor(pos + 1, false);
        }
        return;
    }

    // Coluna hex: cada dígito preenche um nibble; após o segundo, avança.
    bool ok = false;
    const int digit = QString(ch).toInt(&ok, 16);
    if (!ok) return;
    const uint8_t old = data_[cursor_];
    if (!lowNibble_) {
        editByte(cursor_, static_cast<uint8_t>((digit << 4) | (old & 0x0F)));
        lowNibble_ = true;
        anchor_ = cursor_;
        viewport()->update();
    } else {
        editByte(cursor_, static_cast<uint8_t>((old & 0xF0) | digit));
        moveCursor(pos + 1, false);
    }
}
