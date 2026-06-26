include(GNUInstallDirs)

if(TARGET waydisplay_client_sdl)
    install(TARGETS waydisplay_client_sdl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime
    )
endif()

if(TARGET waydisplay_server_wlroots)
    install(TARGETS waydisplay_server_wlroots
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime
    )
endif()

install(FILES
    LICENSE
    README.md
    BUILDING.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT Documentation
)

install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT Documentation
    FILES_MATCHING PATTERN "*.md"
)
