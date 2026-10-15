// =============================================================================
//  DeviceDialog.h — diálogo de seleção do chip
// =============================================================================
//
//  Lista os dispositivos do banco com campo de busca (por nome ou
//  fabricante), como nos gravadores comerciais. Duplo clique ou Enter
//  confirmam a escolha.
// =============================================================================
#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QTableWidget;

class DeviceDialog : public QDialog {
    Q_OBJECT

public:
    // `current` é o nome do chip pré-selecionado.
    explicit DeviceDialog(const QString& current, QWidget* parent = nullptr);

    // Nome do chip escolhido.
    QString selectedDevice() const;

private:
    void applyFilter(const QString& text);
    void updateNotes();

    QLineEdit* search_;
    QTableWidget* table_;
    QLabel* notes_;
};
