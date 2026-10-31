function(GENERATE_VERSION_METADATA)
   # A failed Git lookup is not a clean tree or an uncommitted modification.
   # In either case the build must not emit a missing hash / false -dirty label.
   execute_process(
      COMMAND "${GIT_EXEC}" rev-parse --verify HEAD
      WORKING_DIRECTORY "${SRC_DIR}"
      RESULT_VARIABLE V_HASH_RESULT
      OUTPUT_VARIABLE V_HASH
      ERROR_VARIABLE V_HASH_ERROR
      OUTPUT_STRIP_TRAILING_WHITESPACE )
   if(NOT "${V_HASH_RESULT}" STREQUAL "0")
      message(FATAL_ERROR "Git source identity lookup failed (${V_HASH_RESULT}): ${V_HASH_ERROR}")
   endif()
   string(LENGTH "${V_HASH}" V_HASH_LENGTH)
   if((NOT V_HASH_LENGTH EQUAL 40 AND NOT V_HASH_LENGTH EQUAL 64)
      OR NOT V_HASH MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "Git source identity must be a complete hexadecimal commit ID")
   endif()

   # Compare tracked working-tree AND index changes with HEAD. Untracked build
   # outputs are intentionally excluded, matching Git's normal dirty semantics.
   execute_process(
      COMMAND "${GIT_EXEC}" diff --quiet HEAD --
      WORKING_DIRECTORY "${SRC_DIR}"
      RESULT_VARIABLE V_DIRTY_RESULT
      ERROR_VARIABLE V_DIRTY_ERROR )
   if("${V_DIRTY_RESULT}" STREQUAL "0")
      set(V_DIRTY "false")
   elseif("${V_DIRTY_RESULT}" STREQUAL "1")
      set(V_DIRTY "true")
   else()
      message(FATAL_ERROR "Git tracked-source status lookup failed (${V_DIRTY_RESULT}): ${V_DIRTY_ERROR}")
   endif()

   set(_VERSION_MAJOR_  "${V_MAJOR}")
   set(_VERSION_MINOR_  "${V_MINOR}")
   set(_VERSION_PATCH_  "${V_PATCH}")
   set(_VERSION_SUFFIX_ "${V_SUFFIX}")
   set(_VERSION_HASH_   "${V_HASH}")
   set(_VERSION_SOURCE_LABEL_ "")
   set(_VERSION_DIRTY_  "${V_DIRTY}")
   configure_file(
      "${CUR_SRC_DIR}/src/version_impl.cpp.in"
      "${CUR_BIN_DIR}/src/version_impl.cpp"
      @ONLY )
endfunction(GENERATE_VERSION_METADATA)

GENERATE_VERSION_METADATA()
