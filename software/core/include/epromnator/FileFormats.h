// =============================================================================
//  FileFormats.h — leitura e gravação de imagens em Binário, Intel HEX e S-Record
// =============================================================================
//
//  Todos os carregamentos escrevem sobre um buffer já existente (a imagem do
//  chip, normalmente pré-preenchida com 0xFF). Isso imita o comportamento dos
//  gravadores comerciais: um HEX que só define alguns trechos deixa o resto
//  do buffer intacto.
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace epn {

enum class FileFormat {
    Binary,    // imagem crua (.bin, .rom, .img...)
    IntelHex,  // Intel HEX (.hex, .ihx)
    SRecord,   // Motorola S-Record (.s19, .s28, .s37, .srec, .mot)
};

// Nome do formato para exibição ("Binário", "Intel HEX", "Motorola S-Record").
std::string formatName(FileFormat format);

// Deduz o formato pela extensão do arquivo e, se ela for ambígua, pelo
// conteúdo (primeiro caractere ':' → HEX, 'S' + dígito → S-Record).
FileFormat detectFormat(const std::filesystem::path& path);

// Resumo de um carregamento.
struct LoadResult {
    uint32_t lowestAddress = 0;   // menor endereço escrito no buffer
    uint32_t highestAddress = 0;  // maior endereço escrito no buffer
    size_t bytesLoaded = 0;       // bytes copiados para o buffer
    size_t bytesIgnored = 0;      // bytes fora do buffer (descartados)
};

// Carrega o arquivo para `buffer`.
//  * Binário: o byte 0 do arquivo vai para `buffer[offset]`.
//  * HEX/S-Record: cada registro vai para (endereço do registro + offset).
//    `offset` pode ser negativo, ex.: imagem montada em 0x8000 → offset -0x8000.
// Bytes que caem fora do buffer são contados em `bytesIgnored`.
// Lança FileError em caso de arquivo ilegível ou malformado.
LoadResult loadFile(const std::filesystem::path& path, FileFormat format,
                    std::vector<uint8_t>& buffer, int64_t offset = 0);

// Grava `data` no arquivo. Em HEX/S-Record, `baseAddress` é o endereço do
// primeiro byte (normalmente 0).
void saveFile(const std::filesystem::path& path, FileFormat format,
              std::span<const uint8_t> data, uint32_t baseAddress = 0);

// Versões que trabalham em memória (usadas pelos testes e por loadFile/saveFile).
LoadResult parseIntelHex(const std::string& text, std::vector<uint8_t>& buffer,
                         int64_t offset = 0);
LoadResult parseSRecord(const std::string& text, std::vector<uint8_t>& buffer,
                        int64_t offset = 0);
std::string toIntelHex(std::span<const uint8_t> data, uint32_t baseAddress = 0);
std::string toSRecord(std::span<const uint8_t> data, uint32_t baseAddress = 0);

}  // namespace epn
