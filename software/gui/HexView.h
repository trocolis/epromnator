// =============================================================================
//  HexView.h — editor hexadecimal do buffer (imagem do chip)
// =============================================================================
//
//  Widget próprio, desenhado à mão sobre um QAbstractScrollArea, no layout
//  clássico dos gravadores:
//
//      Endereço  00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII
//      0000      3C 00 A9 FF ...                                   <.......
//
//  Recursos:
//    * edição em hexadecimal ou ASCII (Tab alterna a coluna);
//    * seleção com mouse ou Shift + setas;
//    * destaque de bytes alterados (desde a última leitura/abertura) e de
//      diferenças encontradas na verificação;
//    * bytes 0xFF (vazios) em cor atenuada;
//    * Ctrl+C copia a seleção como texto hexadecimal.
//
//  Desenhar apenas as linhas visíveis mantém o widget leve mesmo com 64 KB.
// =============================================================================
#pragma once

#include <QAbstractScrollArea>

#include <cstdint>
#include <vector>

class HexView : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit HexView(QWidget* parent = nullptr);

    // --- Dados ---------------------------------------------------------------

    // Substitui todo o conteúdo e limpa as marcações.
    void setData(std::vector<uint8_t> data);
    const std::vector<uint8_t>& data() const { return data_; }

    // Escreve `value` em [start, start + length) e marca como alterado.
    void fill(uint32_t start, uint32_t length, uint8_t value);

    // --- Marcações -----------------------------------------------------------

    // Remove as marcas de "alterado" e "diferença".
    void clearMarks();

    // Marca os endereços que divergiram na verificação.
    void setDifferences(const std::vector<uint32_t>& addresses);

    // --- Cursor e seleção ----------------------------------------------------

    void goTo(uint32_t address);
    uint32_t cursorAddress() const { return static_cast<uint32_t>(cursor_); }

    // Seleção atual como [início, fim] inclusivo. Sem seleção, início == fim
    // == cursor.
    uint32_t selectionStart() const;
    uint32_t selectionEnd() const;
    bool hasSelection() const { return anchor_ != cursor_; }

signals:
    // O cursor mudou de posição (ou a seleção mudou).
    void cursorMoved(uint32_t address);

    // O usuário editou algum byte.
    void edited();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    // Bits do vetor de marcações.
    enum Mark : uint8_t {
        Modified = 0x01,    // alterado desde a última leitura/abertura
        Difference = 0x02,  // divergiu na última verificação
    };

    static constexpr int kBytesPerRow = 16;

    // Recalcula as métricas de fonte e as posições das colunas.
    void updateMetrics();
    void updateScrollBars();

    // Coordenada X do byte `column` (0..15) nas colunas hex e ASCII.
    int hexX(int column) const;
    int asciiX(int column) const;

    // Converte um ponto da viewport em índice de byte (-1 se fora).
    // `inAscii` informa em qual coluna o clique ocorreu.
    long long byteAt(const QPoint& pos, bool* inAscii) const;

    // Move o cursor; se `extend`, mantém a âncora (seleção).
    void moveCursor(long long index, bool extend);
    void ensureCursorVisible();

    // Escreve um byte digitado pelo usuário.
    void editByte(size_t index, uint8_t value);

    int visibleRows() const;
    int rowCount() const;

    std::vector<uint8_t> data_;
    std::vector<uint8_t> marks_;

    size_t cursor_ = 0;
    size_t anchor_ = 0;
    bool lowNibble_ = false;   // editando o segundo dígito do byte
    bool asciiFocus_ = false;  // coluna ASCII ativa para edição

    // Métricas (em pixels).
    int charWidth_ = 8;
    int lineHeight_ = 16;
    int addressDigits_ = 4;
    int margin_ = 6;
};
