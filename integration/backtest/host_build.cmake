# Caller-owned inputs supplement the engine's exported native ABI/source facts.
function(quadeplus_bind_financial_host TARGET)
    if(NOT IS_ABSOLUTE "${LEXONGINE_TINYXML2_SOURCE_DIR}"
       OR NOT EXISTS "${LEXONGINE_TINYXML2_SOURCE_DIR}/tinyxml2.h"
       OR NOT EXISTS "${LEXONGINE_TINYXML2_SOURCE_DIR}/tinyxml2.cpp")
        message(
            FATAL_ERROR
                "Financial native identity requires an explicit absolute TinyXML2 source checkout")
    endif()
    file(GLOB_RECURSE HOST_INPUTS CONFIGURE_DEPENDS "${Quadeplus_SOURCE_DIR}/src/quant/*.hpp"
         "${Quadeplus_SOURCE_DIR}/src/quant/*.cpp")
    file(
        GLOB
        HOST_INTEGRATION
        CONFIGURE_DEPENDS
        "${Quadeplus_SOURCE_DIR}/integration/backtest/*.hpp"
        "${Quadeplus_SOURCE_DIR}/integration/backtest/*.cpp"
        "${Quadeplus_SOURCE_DIR}/integration/backtest/*.cmake")
    file(GLOB TINYXML_INPUTS CONFIGURE_DEPENDS "${LEXONGINE_TINYXML2_SOURCE_DIR}/*.h"
         "${LEXONGINE_TINYXML2_SOURCE_DIR}/*.cpp" "${LEXONGINE_TINYXML2_SOURCE_DIR}/CMakeLists.txt")
    list(
        APPEND
        HOST_INPUTS
        ${HOST_INTEGRATION}
        ${TINYXML_INPUTS}
        "${Quadeplus_SOURCE_DIR}/CMakeLists.txt"
        "${Quadeplus_SOURCE_DIR}/integration/backtest/CMakeLists.txt"
        "${Quadeplus_SOURCE_DIR}/tests/backtest_host_tests.cpp"
        "${Quadeplus_SOURCE_DIR}/dependencies/backtesting.lock.json")
    get_target_property(HOST_TARGET_SOURCES ${TARGET} SOURCES)
    get_target_property(HOST_TARGET_DIRECTORY ${TARGET} SOURCE_DIR)
    foreach(INPUT IN LISTS HOST_TARGET_SOURCES)
        if(INPUT MATCHES "^\\$<")
            message(
                FATAL_ERROR
                    "Native host identity requires explicit source files, not opaque generator expressions"
            )
        endif()
        get_filename_component(INPUT_ABSOLUTE "${INPUT}" ABSOLUTE BASE_DIR
                               "${HOST_TARGET_DIRECTORY}")
        list(APPEND HOST_INPUTS "${INPUT_ABSOLUTE}")
    endforeach()
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${QUADEPLUS_LEXONGINE_SOURCE_DIR}"
        OUTPUT_VARIABLE LEX_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${LEXONGINE_TINYXML2_SOURCE_DIR}"
        OUTPUT_VARIABLE XML_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    lexongine_bind_host_build(
        ${TARGET}
        ROOT
        "${Quadeplus_SOURCE_DIR}"
        INPUTS
        ${HOST_INPUTS}
        DEPENDENCY_FACTS
        "Lexongine=${LEX_REV}|TinyXML2=${XML_REV}"
        BUILD_FACTS
        "Quadeplus=${PROJECT_VERSION}|sanitizers=${QUADEPLUS_SANITIZERS}|backtest=${QUADEPLUS_BACKTEST}|compiler=${CMAKE_CXX_COMPILER}|config=${CMAKE_BUILD_TYPE}|flags=${CMAKE_CXX_FLAGS}|bridge=${LEXONGINE_QAEDALUS_BRIDGE}|xml=${LEXONGINE_XML_FRONTEND}"
    )
endfunction()
