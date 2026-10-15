// =============================================================================
//  Testes dos formatos de arquivo (Binário, Intel HEX, S-Record)
// =============================================================================
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

#include "epromnator/Error.h"
#include "epromnator/FileFormats.h"

using namespace epn;
namespace fs = std::filesystem;

namespace {

// Dados de teste: 0x00, 0x01, ... (repete a cada 256).
std::vector<uint8_t> sequence(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i);
    return v;
}

// Arquivo temporário removido automaticamente ao fim do teste.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& name) : path(fs::temp_directory_path() / name) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

}  // namespace

// ---------------------------------------------------------------------------
//  Intel HEX
// ---------------------------------------------------------------------------

// Exemplo clássico da especificação Intel (checksums conhecidos).
TEST(IntelHex, ParsesReferenceRecords) {
    const std::string text =
        ":10010000214601360121470136007EFE09D2190140\n"
        ":100110002146017E17C20001FF5F16002148011928\n"
        ":00000001FF\n";
    std::vector<uint8_t> buffer(0x200, 0xFF);
    const LoadResult r = parseIntelHex(text, buffer);
    EXPECT_EQ(r.bytesLoaded, 32u);
    EXPECT_EQ(r.lowestAddress, 0x100u);
    EXPECT_EQ(r.highestAddress, 0x11Fu);
    EXPECT_EQ(buffer[0x100], 0x21);
    EXPECT_EQ(buffer[0x11F], 0x19);  // último byte de dados do 2º registro
    EXPECT_EQ(buffer[0x0FF], 0xFF);  // fora do arquivo: intacto
}

TEST(IntelHex, RejectsBadChecksum) {
    std::vector<uint8_t> buffer(0x200, 0xFF);
    EXPECT_THROW(parseIntelHex(":10010000214601360121470136007EFE09D2190141\n", buffer), FileError);
}

TEST(IntelHex, RejectsMalformedLine) {
    std::vector<uint8_t> buffer(16);
    EXPECT_THROW(parseIntelHex("10010000\n", buffer), FileError);
    EXPECT_THROW(parseIntelHex(":0Z\n", buffer), FileError);
}

TEST(IntelHex, RoundTrip) {
    const auto data = sequence(1000);
    const std::string text = toIntelHex(data);
    std::vector<uint8_t> buffer(1000, 0xFF);
    const LoadResult r = parseIntelHex(text, buffer);
    EXPECT_EQ(r.bytesLoaded, 1000u);
    EXPECT_EQ(buffer, data);
}

// Endereço linear estendido (registro 04) e offset negativo: imagem montada
// em 0x18000 carregada no início do buffer.
TEST(IntelHex, ExtendedLinearAddressAndNegativeOffset) {
    const auto data = sequence(64);
    const std::string text = toIntelHex(data, 0x18000);
    EXPECT_NE(text.find(":02000004"), std::string::npos);
    std::vector<uint8_t> buffer(64, 0xFF);
    const LoadResult r = parseIntelHex(text, buffer, -0x18000);
    EXPECT_EQ(r.bytesLoaded, 64u);
    EXPECT_EQ(r.bytesIgnored, 0u);
    EXPECT_EQ(buffer, data);
}

TEST(IntelHex, CountsBytesOutsideBuffer) {
    const auto data = sequence(32);
    std::vector<uint8_t> buffer(16, 0xFF);
    const LoadResult r = parseIntelHex(toIntelHex(data), buffer);
    EXPECT_EQ(r.bytesLoaded, 16u);
    EXPECT_EQ(r.bytesIgnored, 16u);
}

// ---------------------------------------------------------------------------
//  S-Record
// ---------------------------------------------------------------------------

// Exemplo da página srec(5) / Wikipédia.
TEST(SRecord, ParsesReferenceRecords) {
    const std::string text =
        "S00F000068656C6C6F202020202000003C\n"
        "S11F00007C0802A6900100049421FFF07C6C1B787C8C23783C6000003863000026\n"
        "S5030001FB\n"
        "S9030000FC\n";
    std::vector<uint8_t> buffer(64, 0xFF);
    const LoadResult r = parseSRecord(text, buffer);
    EXPECT_EQ(r.bytesLoaded, 28u);
    EXPECT_EQ(buffer[0], 0x7C);
    EXPECT_EQ(buffer[27], 0x00);
    EXPECT_EQ(buffer[28], 0xFF);
}

TEST(SRecord, RejectsBadChecksum) {
    std::vector<uint8_t> buffer(64);
    EXPECT_THROW(parseSRecord("S1050000AABB00\n", buffer), FileError);
}

TEST(SRecord, RoundTripS1) {
    const auto data = sequence(300);
    const std::string text = toSRecord(data);
    EXPECT_EQ(text.rfind("S0", 0), 0u);           // começa com cabeçalho
    EXPECT_NE(text.find("\nS1"), std::string::npos);
    EXPECT_NE(text.find("\nS9"), std::string::npos);
    std::vector<uint8_t> buffer(300, 0xFF);
    parseSRecord(text, buffer);
    EXPECT_EQ(buffer, data);
}

TEST(SRecord, UsesS2ForAddressesAbove64K) {
    const auto data = sequence(16);
    const std::string text = toSRecord(data, 0x20000);
    EXPECT_NE(text.find("\nS2"), std::string::npos);
    EXPECT_NE(text.find("\nS8"), std::string::npos);
    std::vector<uint8_t> buffer(16, 0xFF);
    parseSRecord(text, buffer, -0x20000);
    EXPECT_EQ(buffer, data);
}

// ---------------------------------------------------------------------------
//  Arquivos e detecção de formato
// ---------------------------------------------------------------------------

TEST(Files, BinaryRoundTripWithOffset) {
    TempFile f("epn_test.bin");
    const auto data = sequence(100);
    saveFile(f.path, FileFormat::Binary, data);

    std::vector<uint8_t> buffer(200, 0xFF);
    const LoadResult r = loadFile(f.path, FileFormat::Binary, buffer, 50);
    EXPECT_EQ(r.lowestAddress, 50u);
    EXPECT_EQ(r.highestAddress, 149u);
    EXPECT_EQ(buffer[50], 0);
    EXPECT_EQ(buffer[149], 99);
    EXPECT_EQ(buffer[150], 0xFF);
}

TEST(Files, DetectsFormatByExtension) {
    EXPECT_EQ(detectFormat("rom.HEX"), FileFormat::IntelHex);
    EXPECT_EQ(detectFormat("rom.s19"), FileFormat::SRecord);
    EXPECT_EQ(detectFormat("rom.bin"), FileFormat::Binary);
}

TEST(Files, DetectsFormatByContent) {
    TempFile f("epn_test.dat");
    saveFile(f.path, FileFormat::IntelHex, sequence(16));
    EXPECT_EQ(detectFormat(f.path), FileFormat::IntelHex);
    saveFile(f.path, FileFormat::SRecord, sequence(16));
    EXPECT_EQ(detectFormat(f.path), FileFormat::SRecord);
    saveFile(f.path, FileFormat::Binary, sequence(16));
    EXPECT_EQ(detectFormat(f.path), FileFormat::Binary);
}

TEST(Files, MissingFileThrows) {
    std::vector<uint8_t> buffer(16);
    EXPECT_THROW(loadFile("/nao/existe/rom.bin", FileFormat::Binary, buffer), FileError);
}
