# =============================================================================
#  epromnator-cli — ferramenta de linha de comando
# =============================================================================
add_executable(epromnator-cli main.cpp)
target_link_libraries(epromnator-cli PRIVATE epromnator_core)
epn_set_warnings(epromnator-cli)

install(TARGETS epromnator-cli RUNTIME DESTINATION bin)
