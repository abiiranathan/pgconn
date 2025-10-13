cmake_minimum_required(VERSION 3.25)

project(pgpool LANGUAGES C VERSION 1.0.0)

# Options (build system should generally control shared/static)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# Dependencies
find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)

# Library
add_library(pgpool SHARED
    pgpool.c
)

set_target_properties(pgpool PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

target_include_directories(pgpool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pgpool
    PRIVATE
        Threads::Threads
        PostgreSQL::PostgreSQL
)

# Install rules
include(GNUInstallDirs)

install(TARGETS pgpool
    EXPORT pgpool-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    pgpool.h
    pgtypes.h
    pgiter.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pgpool
)

# Export targets for downstream CMake projects (optional but helpful)
install(EXPORT pgpool-targets
    NAMESPACE pgpool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pgpool
)

# pkg-config file generation
set(PKGCONFIG_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pgpool.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pgpool.pc
    DESTINATION ${PKGCONFIG_INSTALL_DIR}
)


