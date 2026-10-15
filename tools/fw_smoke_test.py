#!/usr/bin/env python3
# =============================================================================
#  fw_smoke_test.py — teste rápido do firmware EPROMnator pelo protocolo binário
# =============================================================================
#
#  Implementação mínima do lado PC do protocolo (protocol/epromnator_protocol.h),
#  independente do software C++, útil para validar o firmware recém-gravado.
#
#  Requisitos:  pip install pyserial
#
#  Uso:
#    ./fw_smoke_test.py /dev/ttyUSB0                 # testes não destrutivos
#    ./fw_smoke_test.py /dev/ttyUSB0 --destructive   # grava o chip inteiro!
#
#  Testes não destrutivos: HELLO, GET_INFO, PING, CONFIG (AT28C256), leitura
#  completa do chip e comparação do CRC32 local com o calculado pelo firmware.
#  Teste destrutivo: grava um padrão em todas as páginas, relê e confere.
# =============================================================================
import argparse
import struct
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial não encontrado: pip install pyserial")

# --- Constantes do protocolo (espelho de epromnator_protocol.h) --------------
SOF_REQUEST = 0xA5
SOF_RESPONSE = 0x5A
RESPONSE_FLAG = 0x80

CMD_HELLO = 0x00
CMD_PING = 0x01
CMD_GET_INFO = 0x02
CMD_CONFIG = 0x03
CMD_READ = 0x10
CMD_WRITE = 0x11
CMD_BLANK_CHECK = 0x12
CMD_CRC32 = 0x13

STATUS_NAMES = {
    0: "OK", 1: "ERR_CRC", 2: "ERR_LENGTH", 3: "ERR_UNKNOWN",
    4: "ERR_RANGE", 5: "ERR_TIMEOUT", 6: "ERR_VERIFY", 7: "ERR_PARAM",
}

CHIP_SIZE = 32768   # AT28C256
PAGE_SIZE = 64


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (polinômio 0x1021, valor inicial 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


class Link:
    """Enlace com o firmware: envia requisições e lê respostas."""

    def __init__(self, port: str, baud: int):
        # Abrir a porta reinicia o Nano (DTR); o firmware manda HELLO ao subir.
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.seq = 0

    def read_exact(self, n: int, timeout: float) -> bytes:
        """Lê exatamente n bytes ou levanta TimeoutError."""
        data = b""
        deadline = time.monotonic() + timeout
        while len(data) < n:
            if time.monotonic() > deadline:
                raise TimeoutError(f"esperava {n} bytes, recebeu {len(data)}")
            data += self.ser.read(n - len(data))
        return data

    def read_response(self, timeout: float = 2.0):
        """Lê um quadro de resposta: devolve (cmd, seq, status, payload)."""
        deadline = time.monotonic() + timeout
        # Procura o SOF descartando lixo (ex.: saída do bootloader).
        while True:
            if time.monotonic() > deadline:
                raise TimeoutError("SOF da resposta não chegou")
            b = self.ser.read(1)
            if b and b[0] == SOF_RESPONSE:
                break
        header = self.read_exact(5, timeout)
        cmd, seq, status, length = header[0], header[1], header[2], \
            struct.unpack("<H", header[3:5])[0]
        payload = self.read_exact(length, timeout)
        crc = struct.unpack("<H", self.read_exact(2, timeout))[0]
        if crc != crc16(header + payload):
            raise IOError("CRC da resposta inválido")
        return cmd, seq, status, payload

    def request(self, cmd: int, payload: bytes = b"", timeout: float = 2.0) -> bytes:
        """Envia uma requisição e devolve o payload da resposta (status OK)."""
        self.seq = (self.seq + 1) & 0xFF
        body = bytes([cmd, self.seq]) + struct.pack("<H", len(payload)) + payload
        self.ser.write(bytes([SOF_REQUEST]) + body + struct.pack("<H", crc16(body)))
        rcmd, rseq, status, data = self.read_response(timeout)
        if rcmd != (cmd | RESPONSE_FLAG) or rseq != self.seq:
            raise IOError(f"resposta inesperada cmd={rcmd:#x} seq={rseq}")
        if status != 0:
            extra = f" em {struct.unpack('<I', data)[0]:#06x}" if len(data) == 4 else ""
            raise IOError(f"firmware respondeu {STATUS_NAMES.get(status, status)}{extra}")
        return data


def decode_info(info: bytes) -> str:
    proto, major, minor, patch = info[0], info[1], info[2], info[3]
    max_read = struct.unpack("<H", info[4:6])[0]
    name = info[8:].decode("ascii", "replace")
    return (f"{name} firmware {major}.{minor}.{patch}, protocolo v{proto}, "
            f"READ máx. {max_read}, WRITE máx. {info[6]}, flags {info[7]:#04x}")


def pattern(address: int) -> int:
    """Mesmo padrão do autoteste do shell de depuração."""
    return ((address & 0xFF) ^ (address >> 8) ^ 0x5A) & 0xFF


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    ap.add_argument("port", help="porta serial (ex.: /dev/ttyUSB0, COM3)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--destructive", action="store_true",
                    help="grava um padrão no chip inteiro e confere")
    args = ap.parse_args()

    link = Link(args.port, args.baud)

    # 1) HELLO após o reset (se a placa não reiniciar, seguimos assim mesmo).
    try:
        cmd, _, _, info = link.read_response(timeout=3.0)
        if cmd == (CMD_HELLO | RESPONSE_FLAG):
            print("HELLO:", decode_info(info))
    except TimeoutError:
        print("HELLO não recebido (placa sem auto-reset?), continuando")

    # 2) GET_INFO e PING.
    print("GET_INFO:", decode_info(link.request(CMD_GET_INFO)))
    probe = bytes(range(32))
    assert link.request(CMD_PING, probe) == probe, "eco do PING diferente"
    print("PING: ok")

    # 3) CONFIG para AT28C256: 32 KB, página 64, polling 15 ms, sem SDP.
    link.request(CMD_CONFIG, struct.pack("<IBBB", CHIP_SIZE, PAGE_SIZE, 15, 0))
    print("CONFIG: ok")

    # 4) Teste destrutivo opcional.
    if args.destructive:
        t0 = time.monotonic()
        for a in range(0, CHIP_SIZE, PAGE_SIZE):
            data = bytes(pattern(a + i) for i in range(PAGE_SIZE))
            link.request(CMD_WRITE, struct.pack("<I", a) + data)
        print(f"gravação: {time.monotonic() - t0:.1f} s")

    # 5) Leitura completa.
    t0 = time.monotonic()
    image = bytearray()
    for a in range(0, CHIP_SIZE, 256):
        image += link.request(CMD_READ, struct.pack("<IH", a, 256))
    print(f"leitura: {time.monotonic() - t0:.1f} s")

    # 6) CRC32 local (zlib usa o mesmo CRC-32 IEEE) x CRC32 do firmware.
    local = zlib.crc32(image) & 0xFFFFFFFF
    remote = struct.unpack("<I", link.request(
        CMD_CRC32, struct.pack("<II", 0, CHIP_SIZE), timeout=10.0))[0]
    print(f"CRC32 local {local:08X} / firmware {remote:08X}")
    if local != remote:
        print("FALHA: CRCs diferentes")
        return 1

    if args.destructive:
        errors = sum(1 for a, v in enumerate(image) if v != pattern(a))
        print(f"erros de verificação: {errors}")
        if errors:
            return 1

    print("SUCESSO")
    return 0


if __name__ == "__main__":
    sys.exit(main())
