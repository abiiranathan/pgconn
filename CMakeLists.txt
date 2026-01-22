cmake_minimum_required(VERSION 3.25)
project(pgconn LANGUAGES C VERSION 1.0.0)

add_library(pgconn pgconn.c  pgtypes.c)

set_target_properties(pgconn PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

target_include_directories(pgconn
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pgconn PRIVATE pq pthread)

# Install rules
include(GNUInstallDirs)

install(TARGETS pgconn
    EXPORT pgconn-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    pgconn.h
    pgtypes.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pgconn
)

# Export targets for downstream CMake projects (optional but helpful)
install(EXPORT pgconn-targets
    NAMESPACE pgconn::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgconn
)

# pkg-config file generation
set(PKGCONFIG_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pgconn.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgconn.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pgconn.pc
    DESTINATION ${PKGCONFIG_INSTALL_DIR}
)


