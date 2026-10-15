# =============================================================================
#  EPROMnator — software do PC (biblioteca core, CLI, GUI e testes)
# =============================================================================
#
#  Compilação típica:
#     cmake -S software -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#     ctest --test-dir build --output-on-failure
#
#  Opções:
#     EPN_BUILD_CLI    compila a ferramenta de linha de comando (padrão: ON)
#     EPN_BUILD_TESTS  compila os testes unitários (GTest)     (padrão: ON)
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(epromnator
    VERSION 1.0.0
    DESCRIPTION "Gravador de EEPROMs paralelas 28Cxxx baseado em Arduino Nano"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

option(EPN_BUILD_CLI   "Compila a ferramenta de linha de comando" ON)
option(EPN_BUILD_TESTS "Compila os testes unitários (GTest)"      ON)

# Código compartilhado com o firmware: cabeçalho do protocolo e o
# reconhecedor de quadros (usado pelo emulador).
set(EPN_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
set(EPN_FRAMING_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lib/framing)

# Avisos do compilador para os nossos alvos.
function(epn_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_subdirectory(core)

if(EPN_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(EPN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
