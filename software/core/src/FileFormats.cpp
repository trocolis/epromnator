// =============================================================================
//  FileFormats.cpp — Binário, Intel HEX e Motorola S-Record
// =============================================================================
//
//  Referências:
//   * Intel HEX: "Hexadecimal Object File Format Specification", Intel, 1988.
//   * S-Record: manual srec(5) / Motorola M68000 Family Programmer's Reference.
// =============================================================================
#include "epromnator/FileFormats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "epromnator/Error.h"

namespace epn {

namespace {

// ---------------------------------------------------------------------------
//  Utilitários de arquivo
// ---------------------------------------------------------------------------

// Lê o arquivo inteiro para uma string (modo binário).
std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw FileError("Não foi possível abrir o arquivo: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeWholeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw FileError("Não foi possível criar o arquivo: " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw FileError("Falha ao gravar o arquivo: " + path.string());
}

// Converte extensão para minúsculas (".HEX" → ".hex").
std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// ---------------------------------------------------------------------------
//  Utilitários de texto hexadecimal
// ---------------------------------------------------------------------------

// Valor de um dígito hexadecimal, ou -1 se não for um.
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Converte uma sequência de pares hexadecimais em bytes.
// Retorna false se houver caracteres inválidos ou quantidade ímpar.
bool decodeHexBytes(std::string_view text, std::vector<uint8_t>& out) {
    if (text.size() % 2 != 0) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = hexDigit(text[i]);
        const int lo = hexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Acrescenta um byte em hexadecimal (maiúsculo) ao texto.
void appendHex(std::string& out, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    out += digits[value >> 4];
    out += digits[value & 0x0F];
}

// Remove espaços e CR/LF das pontas de uma linha.
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Lança FileError com o número da linha.
[[noreturn]] void lineError(size_t line, const std::string& message) {
    throw FileError("Linha " + std::to_string(line) + ": " + message);
}

// Copia um byte para o buffer (se couber) e atualiza o resumo.
void store(std::vector<uint8_t>& buffer, int64_t address, uint8_t value, LoadResult& result) {
    if (address < 0 || address >= static_cast<int64_t>(buffer.size())) {
        ++result.bytesIgnored;
        return;
    }
    const auto a = static_cast<uint32_t>(address);
    buffer[a] = value;
    if (result.bytesLoaded == 0) {
        result.lowestAddress = result.highestAddress = a;
    } else {
        result.lowestAddress = std::min(result.lowestAddress, a);
        result.highestAddress = std::max(result.highestAddress, a);
    }
    ++result.bytesLoaded;
}

// Percorre o texto linha a linha chamando `fn(numeroDaLinha, linha)` para
// cada linha não vazia. `fn` retorna false para interromper (fim de arquivo).
template <typename Fn>
void forEachLine(const std::string& text, Fn fn) {
    std::istringstream in(text);
    std::string raw;
    size_t number = 0;
    while (std::getline(in, raw)) {
        ++number;
        const std::string_view line = trim(raw);
        if (line.empty()) continue;
        if (!fn(number, line)) break;
    }
}

}  // namespace

// =============================================================================
//  Detecção de formato
// =============================================================================

std::string formatName(FileFormat format) {
    switch (format) {
        case FileFormat::Binary:   return "Binário";
        case FileFormat::IntelHex: return "Intel HEX";
        case FileFormat::SRecord:  return "Motorola S-Record";
    }
    return "?";
}

FileFormat detectFormat(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext == ".hex" || ext == ".ihx" || ext == ".ihex" || ext == ".h86") {
        return FileFormat::IntelHex;
    }
    if (ext == ".s19" || ext == ".s28" || ext == ".s37" || ext == ".srec" ||
        ext == ".mot" || ext == ".sx" || ext == ".s") {
        return FileFormat::SRecord;
    }
    if (ext == ".bin" || ext == ".rom" || ext == ".img") return FileFormat::Binary;

    // Extensão desconhecida: olha o começo do arquivo. Arquivos de texto
    // HEX/S-Record começam com ':' ou 'S' seguido de dígito.
    std::ifstream in(path, std::ios::binary);
    char head[2] = {0, 0};
    in.read(head, 2);
    if (head[0] == ':' && hexDigit(head[1]) >= 0) return FileFormat::IntelHex;
    if (head[0] == 'S' && head[1] >= '0' && head[1] <= '9') return FileFormat::SRecord;
    return FileFormat::Binary;
}

// =============================================================================
//  Intel HEX
// =============================================================================
//
//  Formato de cada registro:  :LLAAAATT[DD...]CC
//    LL   quantidade de bytes de dados
//    AAAA endereço (16 bits) do primeiro byte
//    TT   tipo: 00 dados, 01 fim, 02 segmento estendido, 03 início segmento,
//              04 endereço linear estendido, 05 início linear
//    CC   complemento de dois da soma de todos os bytes anteriores
// =============================================================================

LoadResult parseIntelHex(const std::string& text, std::vector<uint8_t>& buffer, int64_t offset) {
    LoadResult result;
    uint32_t base = 0;        // base vinda dos registros 02/04
    bool sawEof = false;
    std::vector<uint8_t> bytes;

    forEachLine(text, [&](size_t line, std::string_view s) {
        if (s.front() != ':') lineError(line, "registro não começa com ':'");
        if (!decodeHexBytes(s.substr(1), bytes) || bytes.size() < 5) {
            lineError(line, "caracteres hexadecimais inválidos ou registro curto");
        }

        const uint8_t length = bytes[0];
        if (bytes.size() != static_cast<size_t>(length) + 5) {
            lineError(line, "tamanho do registro não confere com o campo LL");
        }

        // A soma de todos os bytes (incluindo o checksum) deve ser zero.
        uint8_t sum = 0;
        for (uint8_t b : bytes) sum = static_cast<uint8_t>(sum + b);
        if (sum != 0) lineError(line, "checksum inválido");

        const uint16_t address = static_cast<uint16_t>((bytes[1] << 8) | bytes[2]);
        const uint8_t type = bytes[3];
        const uint8_t* data = &bytes[4];

        switch (type) {
            case 0x00:  // dados
                for (uint8_t i = 0; i < length; ++i) {
                    store(buffer, static_cast<int64_t>(base) + address + i + offset, data[i], result);
                }
                break;
            case 0x01:  // fim de arquivo
                sawEof = true;
                return false;
            case 0x02:  // endereço de segmento estendido (base = segmento * 16)
                if (length != 2) lineError(line, "registro 02 deve ter 2 bytes");
                base = static_cast<uint32_t>((data[0] << 8) | data[1]) << 4;
                break;
            case 0x04:  // endereço linear estendido (16 bits superiores)
                if (length != 2) lineError(line, "registro 04 deve ter 2 bytes");
                base = static_cast<uint32_t>((data[0] << 8) | data[1]) << 16;
                break;
            case 0x03:  // endereço de início (CS:IP) — irrelevante para EEPROM
            case 0x05:  // endereço de início linear — idem
                break;
            default:
                lineError(line, "tipo de registro desconhecido");
        }
        return true;
    });

    // Arquivos sem o registro de fim são aceitos se tiverem dados (há
    // ferramentas que o omitem), mas um arquivo vazio é erro.
    if (!sawEof && result.bytesLoaded == 0 && result.bytesIgnored == 0) {
        throw FileError("Arquivo Intel HEX sem dados");
    }
    return result;
}

std::string toIntelHex(std::span<const uint8_t> data, uint32_t baseAddress) {
    constexpr size_t kBytesPerLine = 16;
    std::string out;
    uint32_t currentUpper = 0;  // 16 bits superiores já informados

    // Emite um registro completo, calculando o checksum.
    auto emit = [&](uint8_t type, uint16_t address, std::span<const uint8_t> payload) {
        uint8_t sum = static_cast<uint8_t>(payload.size() + (address >> 8) + (address & 0xFF) + type);
        out += ':';
        appendHex(out, static_cast<uint8_t>(payload.size()));
        appendHex(out, static_cast<uint8_t>(address >> 8));
        appendHex(out, static_cast<uint8_t>(address & 0xFF));
        appendHex(out, type);
        for (uint8_t b : payload) {
            appendHex(out, b);
            sum = static_cast<uint8_t>(sum + b);
        }
        appendHex(out, static_cast<uint8_t>(-sum));
        out += '\n';
    };

    for (size_t i = 0; i < data.size();) {
        const uint32_t address = baseAddress + static_cast<uint32_t>(i);
        const uint32_t upper = address >> 16;

        // Mudou a janela de 64 KB: registro 04 com os 16 bits superiores.
        if (upper != currentUpper) {
            const uint8_t ext[2] = {static_cast<uint8_t>(upper >> 8), static_cast<uint8_t>(upper)};
            emit(0x04, 0, ext);
            currentUpper = upper;
        }

        // Uma linha não pode atravessar o limite de 64 KB.
        size_t n = std::min(kBytesPerLine, data.size() - i);
        n = std::min<size_t>(n, 0x10000 - (address & 0xFFFF));
        emit(0x00, static_cast<uint16_t>(address & 0xFFFF), data.subspan(i, n));
        i += n;
    }
    emit(0x01, 0, {});  // fim de arquivo
    return out;
}

// =============================================================================
//  Motorola S-Record
// =============================================================================
//
//  Formato de cada registro:  S T CC AAAA... DD... KK
//    T    tipo (0 cabeçalho, 1/2/3 dados com endereço de 16/24/32 bits,
//              5/6 contagem, 7/8/9 fim com endereço de 32/24/16 bits)
//    CC   quantidade de bytes a seguir (endereço + dados + checksum)
//    KK   complemento de um da soma de CC, endereço e dados
// =============================================================================

LoadResult parseSRecord(const std::string& text, std::vector<uint8_t>& buffer, int64_t offset) {
    LoadResult result;
    std::vector<uint8_t> bytes;
    bool sawData = false;

    forEachLine(text, [&](size_t line, std::string_view s) {
        if (s.size() < 4 || s[0] != 'S' || s[1] < '0' || s[1] > '9') {
            lineError(line, "registro não começa com 'S' seguido do tipo");
        }
        const int type = s[1] - '0';
        if (!decodeHexBytes(s.substr(2), bytes) || bytes.empty()) {
            lineError(line, "caracteres hexadecimais inválidos");
        }

        const uint8_t count = bytes[0];
        if (bytes.size() != static_cast<size_t>(count) + 1) {
            lineError(line, "tamanho do registro não confere com o campo de contagem");
        }

        // Soma de todos os bytes exceto o checksum; o checksum é o complemento.
        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < bytes.size(); ++i) sum = static_cast<uint8_t>(sum + bytes[i]);
        if (static_cast<uint8_t>(~sum) != bytes.back()) lineError(line, "checksum inválido");

        // Quantidade de bytes de endereço de cada tipo.
        size_t addressBytes = 0;
        switch (type) {
            case 0: case 1: case 5: case 9: addressBytes = 2; break;
            case 2: case 6: case 8:         addressBytes = 3; break;
            case 3: case 7:                 addressBytes = 4; break;
            default: lineError(line, "tipo de registro S" + std::to_string(type) + " inválido");
        }
        if (count < addressBytes + 1) lineError(line, "registro curto demais");

        uint32_t address = 0;
        for (size_t i = 0; i < addressBytes; ++i) address = (address << 8) | bytes[1 + i];

        if (type >= 1 && type <= 3) {
            const size_t dataStart = 1 + addressBytes;
            const size_t dataLength = count - addressBytes - 1;
            for (size_t i = 0; i < dataLength; ++i) {
                store(buffer, static_cast<int64_t>(address) + static_cast<int64_t>(i) + offset,
                      bytes[dataStart + i], result);
            }
            sawData = true;
        } else if (type >= 7) {
            return false;  // registro de término
        }
        // S0 (cabeçalho) e S5/S6 (contagem) são apenas informativos.
        return true;
    });

    if (!sawData) throw FileError("Arquivo S-Record sem registros de dados");
    return result;
}

std::string toSRecord(std::span<const uint8_t> data, uint32_t baseAddress) {
    constexpr size_t kBytesPerLine = 16;
    std::string out;

    // Escolhe o menor tipo de registro que comporta o maior endereço.
    const uint64_t last = data.empty() ? baseAddress : baseAddress + data.size() - 1;
    int dataType = 1;
    size_t addressBytes = 2;
    if (last > 0xFFFFFF) {
        dataType = 3;
        addressBytes = 4;
    } else if (last > 0xFFFF) {
        dataType = 2;
        addressBytes = 3;
    }

    auto emit = [&](int type, size_t addrBytes, uint32_t address, std::span<const uint8_t> payload) {
        const auto count = static_cast<uint8_t>(addrBytes + payload.size() + 1);
        uint8_t sum = count;
        out += 'S';
        out += static_cast<char>('0' + type);
        appendHex(out, count);
        for (size_t i = addrBytes; i-- > 0;) {
            const auto b = static_cast<uint8_t>(address >> (8 * i));
            appendHex(out, b);
            sum = static_cast<uint8_t>(sum + b);
        }
        for (uint8_t b : payload) {
            appendHex(out, b);
            sum = static_cast<uint8_t>(sum + b);
        }
        appendHex(out, static_cast<uint8_t>(~sum));
        out += '\n';
    };

    // Cabeçalho S0 com o nome do programa.
    static constexpr uint8_t kHeader[] = {'e', 'p', 'r', 'o', 'm', 'n', 'a', 't', 'o', 'r'};
    emit(0, 2, 0, kHeader);

    size_t records = 0;
    for (size_t i = 0; i < data.size(); i += kBytesPerLine) {
        const size_t n = std::min(kBytesPerLine, data.size() - i);
        emit(dataType, addressBytes, baseAddress + static_cast<uint32_t>(i), data.subspan(i, n));
        ++records;
    }

    // Registro de contagem (S5) quando cabe em 16 bits.
    if (records <= 0xFFFF) emit(5, 2, static_cast<uint32_t>(records), {});

    // Registro de término correspondente ao tipo de dados (S9/S8/S7).
    emit(10 - dataType, addressBytes, 0, {});
    return out;
}

// =============================================================================
//  Arquivos
// =============================================================================

LoadResult loadFile(const std::filesystem::path& path, FileFormat format,
                    std::vector<uint8_t>& buffer, int64_t offset) {
    const std::string content = readWholeFile(path);
    switch (format) {
        case FileFormat::IntelHex:
            return parseIntelHex(content, buffer, offset);
        case FileFormat::SRecord:
            return parseSRecord(content, buffer, offset);
        case FileFormat::Binary: {
            if (content.empty()) throw FileError("Arquivo vazio: " + path.string());
            LoadResult result;
            for (size_t i = 0; i < content.size(); ++i) {
                store(buffer, static_cast<int64_t>(i) + offset, static_cast<uint8_t>(content[i]),
                      result);
            }
            return result;
        }
    }
    return {};
}

void saveFile(const std::filesystem::path& path, FileFormat format,
              std::span<const uint8_t> data, uint32_t baseAddress) {
    switch (format) {
        case FileFormat::IntelHex:
            writeWholeFile(path, toIntelHex(data, baseAddress));
            break;
        case FileFormat::SRecord:
            writeWholeFile(path, toSRecord(data, baseAddress));
            break;
        case FileFormat::Binary:
            writeWholeFile(path, std::string(data.begin(), data.end()));
            break;
    }
}

}  // namespace epn
