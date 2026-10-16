// =============================================================================
//  DeviceDialog.cpp
// =============================================================================
#include "DeviceDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "epromnator/Device.h"

DeviceDialog::DeviceDialog(const QString& current, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Selecionar chip"));
    resize(620, 420);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Buscar por nome, tipo ou fabricante..."));
    search_->setClearButtonEnabled(true);

    // Tabela com uma linha por dispositivo do banco.
    const auto devices = epn::deviceDatabase();
    table_ = new QTableWidget(static_cast<int>(devices.size()), 6, this);
    table_->setHorizontalHeaderLabels(
        {tr("Chip"), tr("Tipo"), tr("Fabricante"), tr("Tamanho"), tr("Página"), tr("SDP")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true);

    for (int row = 0; row < static_cast<int>(devices.size()); ++row) {
        const epn::Device& d = devices[static_cast<size_t>(row)];
        table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(d.name)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(epn::chipTypeName(d.type))));
        table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(d.manufacturer)));
        table_->setItem(row, 3, new QTableWidgetItem(tr("%1 KB").arg(d.size / 1024)));
        table_->setItem(row, 4, new QTableWidgetItem(d.pageSize > 1 ? QString::number(d.pageSize)
                                                                    : tr("byte")));
        table_->setItem(row, 5, new QTableWidgetItem(d.supportsSdp ? tr("sim") : tr("não")));
        if (QString::fromStdString(d.name) == current) table_->selectRow(row);
    }
    table_->resizeColumnsToContents();
    if (table_->selectedItems().isEmpty()) table_->selectRow(0);

    // Observações do chip selecionado.
    notes_ = new QLabel(this);
    notes_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(search_);
    layout->addWidget(table_, 1);
    layout->addWidget(notes_);
    layout->addWidget(buttons);

    connect(search_, &QLineEdit::textChanged, this, &DeviceDialog::applyFilter);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &DeviceDialog::updateNotes);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateNotes();
    search_->setFocus();
}

QString DeviceDialog::selectedDevice() const {
    const auto rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return {};
    return table_->item(rows.first().row(), 0)->text();
}

void DeviceDialog::applyFilter(const QString& text) {
    // Esconde as linhas que não contêm o texto no nome ou no fabricante.
    int firstVisible = -1;
    for (int row = 0; row < table_->rowCount(); ++row) {
        const bool match = text.isEmpty() ||
                           table_->item(row, 0)->text().contains(text, Qt::CaseInsensitive) ||
                           table_->item(row, 1)->text().contains(text, Qt::CaseInsensitive) ||
                           table_->item(row, 2)->text().contains(text, Qt::CaseInsensitive);
        table_->setRowHidden(row, !match);
        if (match && firstVisible < 0) firstVisible = row;
    }
    // Mantém sempre uma linha visível selecionada (Enter confirma direto).
    const auto rows = table_->selectionModel()->selectedRows();
    if (firstVisible >= 0 && (rows.isEmpty() || table_->isRowHidden(rows.first().row()))) {
        table_->selectRow(firstVisible);
    }
}

void DeviceDialog::updateNotes() {
    const epn::Device* d = epn::findDevice(selectedDevice().toStdString());
    notes_->setText(d ? QString::fromStdString(d->notes) : QString());
}
