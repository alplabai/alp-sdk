cmake_minimum_required(VERSION 3.27)
include(ExternalProject)
	
project("examples" NONE)

# Roots
include("roots.cmake")

# Context specific lists
set(CONTEXTS
  "blinky_alp.Debug+Alif-E7-DevKit"
  "hello_world_alp.Debug+Alif-E7-DevKit"
  "alp_spi_demo.Debug+Alif-E7-DevKit"
)
list(LENGTH CONTEXTS CONTEXTS_LENGTH)
math(EXPR CONTEXTS_LENGTH "${CONTEXTS_LENGTH}-1")

set(DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/blinky_alp.Debug+Alif-E7-DevKit"
  "${CMAKE_CURRENT_SOURCE_DIR}/hello_world_alp.Debug+Alif-E7-DevKit"
  "${CMAKE_CURRENT_SOURCE_DIR}/alp_spi_demo.Debug+Alif-E7-DevKit"
)

set(OUTPUTS_1
  "${SOLUTION_ROOT}/out/blinky_alp/Alif-E7-DevKit/Debug/blinky_alp.bin"
  "${SOLUTION_ROOT}/out/blinky_alp/Alif-E7-DevKit/Debug/blinky_alp.axf"
)
set(OUTPUTS_2
  "${SOLUTION_ROOT}/out/hello_world_alp/Alif-E7-DevKit/Debug/hello_world_alp.bin"
  "${SOLUTION_ROOT}/out/hello_world_alp/Alif-E7-DevKit/Debug/hello_world_alp.axf"
)
set(OUTPUTS_3
  "${SOLUTION_ROOT}/out/alp_spi_demo/Alif-E7-DevKit/Debug/alp_spi_demo.axf"
)

set(ARGS
  "-DSOLUTION_ROOT=${SOLUTION_ROOT}"
  "-DCMSIS_PACK_ROOT=${CMSIS_PACK_ROOT}"
  "-DCMSIS_COMPILER_ROOT=${CMSIS_COMPILER_ROOT}"
)

# Compilation database
add_custom_target(database)

# Iterate over contexts
foreach(INDEX RANGE ${CONTEXTS_LENGTH})

  math(EXPR N "${INDEX}+1")
  list(GET CONTEXTS ${INDEX} CONTEXT)
  list(GET DIRS ${INDEX} DIR)

  # Create external project, set configure and build steps
  ExternalProject_Add(${CONTEXT}
    PREFIX                ${DIR}
    SOURCE_DIR            ${DIR}
    BINARY_DIR            ${N}
    INSTALL_COMMAND       ""
    TEST_COMMAND          ""
    CONFIGURE_COMMAND     ${CMAKE_COMMAND} -G Ninja -S <SOURCE_DIR> -B <BINARY_DIR> ${ARGS} 
    BUILD_COMMAND         ${CMAKE_COMMAND} -E echo "Building CMake target '${CONTEXT}'"
    COMMAND               ${CMAKE_COMMAND} --build <BINARY_DIR>
    BUILD_ALWAYS          TRUE
    BUILD_BYPRODUCTS      ${OUTPUTS_${N}}
    LOG_CONFIGURE         ON
    LOG_OUTPUT_ON_FAILURE ON
    USES_TERMINAL_BUILD   ON
  )

  # Executes command step
  ExternalProject_Add_Step(${CONTEXT} executes
    DEPENDEES         build
  )

  ExternalProject_Add_StepTargets(${CONTEXT} build configure executes)

  # Debug
  message(VERBOSE "Configure Context: ${CMAKE_COMMAND} -G Ninja -S ${DIR} -B ${N}")

  # Database generation step
  ExternalProject_Add_Step(${CONTEXT} database
    COMMAND           ${CMAKE_COMMAND} --build <BINARY_DIR> --target database
    ALWAYS            TRUE
    LOG               TRUE
    USES_TERMINAL     ON
    DEPENDEES         configure
  )
  ExternalProject_Add_StepTargets(${CONTEXT} database)
  add_dependencies(database ${CONTEXT}-database)

endforeach()
