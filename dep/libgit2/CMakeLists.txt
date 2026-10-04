# Set some defaults.
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE)
set(BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(STATIC_CRT
    OFF
    CACHE BOOL "" FORCE)

if(WIN32)
  # Disable search for OpenSSL.
  set(USE_OPENSSL
      OFF
      CACHE BOOL "" FORCE)

  # Set variables manually.
  set(OPENSSL_FOUND TRUE)
  set(OPENSSL_INCLUDE_DIR ${OPENSSL_DIR}/inc32)
  set(OPENSSL_LIBRARIES ${OPENSSL_DIR}/out32dll/libeay32.lib
                        ${OPENSSL_DIR}/out32dll/ssleay32.lib)

  # Disable nanosecond precision.
  set(USE_NSEC
      OFF
      CACHE BOOL "" FORCE)
endif()

if(USE_SYSTEM_LIBGIT2)
  pkg_check_modules(LIBGIT2 REQUIRED libgit2>=1.9)
else()
  set(BUILD_CLI OFF)

  # Fixes that the libgit2 of the submodule lacks, taken from newer versions
  # or not yet upstream. A patch that is already applied, e.g. by updating
  # the submodule, is skipped.
  find_package(Git REQUIRED)
  file(GLOB LIBGIT2_PATCHES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch)
  list(SORT LIBGIT2_PATCHES)
  foreach(PATCH ${LIBGIT2_PATCHES})
    set_property(
      DIRECTORY
      APPEND
      PROPERTY CMAKE_CONFIGURE_DEPENDS ${PATCH})
    execute_process(
      COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${PATCH}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libgit2
      RESULT_VARIABLE APPLIED
      OUTPUT_QUIET ERROR_QUIET)
    if(NOT APPLIED EQUAL 0)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} apply ${PATCH}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libgit2
        RESULT_VARIABLE RESULT
        ERROR_VARIABLE ERROR)
      if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to apply ${PATCH}: ${ERROR}")
      endif()
      message(STATUS "Applied ${PATCH}")
    endif()
  endforeach()

  add_subdirectory(libgit2)

  # Include the generated experimental.h
  target_include_directories(
    libgit2 INTERFACE ${PROJECT_BINARY_DIR}/dep/libgit2/libgit2/include/git2)
endif()
