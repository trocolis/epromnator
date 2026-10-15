# =============================================================================
#  epromnator-gui — interface gráfica (Qt 6 Widgets)
# =============================================================================
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(epromnator-gui WIN32 MACOSX_BUNDLE
    main.cpp
    MainWindow.cpp
    MainWindow.h
    HexView.cpp
    HexView.h
    DeviceDialog.cpp
    DeviceDialog.h
    ProgrammerWorker.cpp
    ProgrammerWorker.h
)

target_link_libraries(epromnator-gui PRIVATE epromnator_core Qt6::Widgets)
epn_set_warnings(epromnator-gui)

set_target_properties(epromnator-gui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "EPROMnator"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.epromnator"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

install(TARGETS epromnator-gui
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)
