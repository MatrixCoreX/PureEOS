# Exported source still has a real project version. Its identity must not be
# misrepresented as a Git commit, or silently discarded as "unknown".
set(PUREEOS_SOURCE_ID "" CACHE STRING "SHA256 of the verified exported-source manifest")
if(PUREEOS_SOURCE_ID)
   string(LENGTH "${PUREEOS_SOURCE_ID}" _source_id_length)
   if(NOT _source_id_length EQUAL 64 OR NOT PUREEOS_SOURCE_ID MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "PUREEOS_SOURCE_ID must be a lowercase 64-character SHA256")
   endif()
   if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
      message(FATAL_ERROR "PUREEOS_SOURCE_ID is for exported source only; Git checkouts use their real commit")
   endif()
   set(_VERSION_HASH_ "${PUREEOS_SOURCE_ID}")
   set(_VERSION_SOURCE_LABEL_ "src-")
else()
   set(_VERSION_HASH_ "")
   set(_VERSION_SOURCE_LABEL_ "unidentified-source")
endif()
set(_VERSION_MAJOR_  "${VERSION_MAJOR}")
set(_VERSION_MINOR_  "${VERSION_MINOR}")
set(_VERSION_PATCH_  "${VERSION_PATCH}")
set(_VERSION_SUFFIX_ "${VERSION_SUFFIX}")
set(_VERSION_DIRTY_  "false")

# Construct the library target.
add_library(
   version
   "${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp"
   "${CMAKE_CURRENT_BINARY_DIR}/src/version_impl.cpp")

# Make dependencies visible to the given target library to be constructed.
target_include_directories(
   version
   PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include/"
   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/" )

# Create a custom target to update the version metadata upon every build.
find_package(Git)
if(EXISTS ${CMAKE_SOURCE_DIR}/.git AND ${GIT_FOUND})
   add_custom_target(
      evaluate_every_build ALL
      COMMAND ${CMAKE_COMMAND} -DGIT_EXEC=${GIT_EXECUTABLE}
                               -DCUR_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
                               -DCUR_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                               -DSRC_DIR=${CMAKE_SOURCE_DIR}
                               -DV_MAJOR=${VERSION_MAJOR}
                               -DV_MINOR=${VERSION_MINOR}
                               -DV_PATCH=${VERSION_PATCH}
                               -DV_SUFFIX=${VERSION_SUFFIX}
                               -P ${CMAKE_SOURCE_DIR}/CMakeModules/VersionUtils.cmake
      BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/src/version_impl.cpp
      COMMENT "Updating version metadata..." VERBATIM )
                             
      # Create a dependency for the given library target.
      add_dependencies(version evaluate_every_build)
else()
   # Modify and substitute the `.cpp.in` file for a `.cpp` in the build directory.
   configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/src/version_impl.cpp.in
      ${CMAKE_CURRENT_BINARY_DIR}/src/version_impl.cpp
      @ONLY )
endif()
