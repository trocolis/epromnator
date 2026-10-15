# =============================================================================
#  Testes unitários (GoogleTest)
# =============================================================================
#
#  Usa o GTest instalado no sistema; se não houver, baixa via FetchContent
#  (útil no Windows/macOS e na CI).
# =============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # No Windows, usa o mesmo runtime (/MD) do restante do projeto.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # Não instala o GTest junto com o programa (cmake --install / AppImage).
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(epromnator_tests
    test_checksum.cpp
    test_devices.cpp
    test_file_formats.cpp
    test_programmer.cpp
    test_protocol_client.cpp
)

target_link_libraries(epromnator_tests PRIVATE epromnator_core GTest::gtest_main)
epn_set_warnings(epromnator_tests)

include(GoogleTest)
gtest_discover_tests(epromnator_tests)
