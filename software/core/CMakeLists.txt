# =============================================================================
#  libepromnator_core — lógica do gravador, sem dependência de interface
# =============================================================================

# Versão do software disponível no código via <epromnator/Version.h>.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/epromnator/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/epromnator/Version.h
    @ONLY)

add_library(epromnator_core STATIC
    src/Checksum.cpp
    src/DeviceDatabase.cpp
    src/Emulator.cpp
    src/Error.cpp
    src/FileFormats.cpp
    src/Programmer.cpp
    src/ProtocolClient.cpp
    src/SerialPort.cpp
    # Reconhecedor de quadros do firmware, reaproveitado pelo emulador.
    ${EPN_FRAMING_DIR}/FrameParser.cpp
)

target_include_directories(epromnator_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${EPN_PROTOCOL_DIR}
    PRIVATE
        ${EPN_FRAMING_DIR})

epn_set_warnings(epromnator_core)
