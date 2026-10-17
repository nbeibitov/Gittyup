add_executable(gittyup-editor WIN32 main.cpp)
target_link_libraries(gittyup-editor Qt6::Network)

set_target_properties(gittyup-editor PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                $<TARGET_FILE_DIR:gittyup>)

if(APPLE)
  set_target_properties(
    gittyup-editor PROPERTIES INSTALL_RPATH
                              "/usr/local/qt;@loader_path/../Frameworks")
else()
  if(NOT WIN32 AND NOT FLATPAK)
    set_target_properties(gittyup-editor PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()

  install(
    TARGETS gittyup-editor
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ${GITTYUP_NAME})
endif()
