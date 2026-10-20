if(NOT QUADEPLUS_LEXONGINE_SOURCE_DIR
   OR NOT EXISTS "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/CMakeLists.txt"
   OR NOT EXISTS "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/src/lexongine/source.hpp")
    message(
        FATAL_ERROR
            "QUADEPLUS_BACKTEST requires an explicit compatible QUADEPLUS_LEXONGINE_SOURCE_DIR")
endif()
if(LEXONGINE_QAEDALUS_BRIDGE)
    message(FATAL_ERROR "QUADEPLUS_BACKTEST requires LEXONGINE_QAEDALUS_BRIDGE=OFF")
endif()
set(LEXONGINE_QAEDALUS_BRIDGE
    OFF
    CACHE BOOL "Daily research does not use the Trade/storage bridge")
if(NOT DEFINED LEXONGINE_XML_FRONTEND)
    if(DEFINED LEXONGINE_TINYXML2_SOURCE_DIR OR DEFINED tinyxml2_DIR)
        set(LEXONGINE_XML_FRONTEND
            ON
            CACHE BOOL "Enable native backtest CLI with explicitly supplied XML dependency")
    else()
        set(LEXONGINE_XML_FRONTEND
            OFF
            CACHE BOOL "Dataset/accounting profile without an XML dependency")
    endif()
endif()

foreach(TYPE_HEADER observations.hpp reference.hpp validation.hpp value_types.hpp)
    if(NOT EXISTS "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/src/quant/data/${TYPE_HEADER}")
        message(FATAL_ERROR "External Lexongine lacks canonical header: ${TYPE_HEADER}")
    endif()
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${Quadeplus_SOURCE_DIR}/src/quant/data/${TYPE_HEADER}"
                 "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/src/quant/data/${TYPE_HEADER}")
    file(SHA256 "${Quadeplus_SOURCE_DIR}/src/quant/data/${TYPE_HEADER}" QUADEPLUS_TYPE_DIGEST)
    file(SHA256 "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/src/quant/data/${TYPE_HEADER}"
         LEXONGINE_TYPE_DIGEST)
    if(NOT QUADEPLUS_TYPE_DIGEST STREQUAL LEXONGINE_TYPE_DIGEST)
        message(FATAL_ERROR "Quadeplus/Lexongine canonical headers differ: ${TYPE_HEADER}")
    endif()
endforeach()

# Add the external root exactly once, outside either source tree. Existing targets
# from a different inclusion would make the explicit checkout ambiguous.
if(TARGET lexongine_core OR TARGET lexongine_quant_components)
    message(
        FATAL_ERROR
            "Lexongine already added; the backtest profile owns its single external-root inclusion")
endif()
add_subdirectory("${QUADEPLUS_LEXONGINE_SOURCE_DIR}"
                 "${CMAKE_CURRENT_BINARY_DIR}/lexongine-external")

add_library(
    quadeplus_daily_research STATIC "${Quadeplus_SOURCE_DIR}/src/quant/research/daily_dataset.cpp"
                                    "${Quadeplus_SOURCE_DIR}/src/quant/research/daily_source.cpp")
target_link_libraries(quadeplus_daily_research PUBLIC Quadeplus::core lexongine_core
                                                      lexongine_quant_components)
target_compile_options(quadeplus_daily_research PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(backtest_dataset_tests "${Quadeplus_SOURCE_DIR}/tests/backtest_dataset_tests.cpp"
                                      "${Quadeplus_SOURCE_DIR}/tests/backtest_source_checks.cpp")
target_link_libraries(backtest_dataset_tests PRIVATE quadeplus_daily_research)
target_compile_options(backtest_dataset_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME backtest_dataset COMMAND backtest_dataset_tests
                                       "${CMAKE_CURRENT_BINARY_DIR}/dataset-scratch")

add_library(
    quadeplus_backtest_accounting STATIC
    "${Quadeplus_SOURCE_DIR}/src/quant/backtest/arithmetic.cpp"
    "${Quadeplus_SOURCE_DIR}/src/quant/backtest/accounting.cpp")
target_link_libraries(quadeplus_backtest_accounting PUBLIC quadeplus_daily_research)
target_compile_options(quadeplus_backtest_accounting PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(backtest_accounting_tests
               "${Quadeplus_SOURCE_DIR}/tests/backtest_accounting_tests.cpp")
target_link_libraries(backtest_accounting_tests PRIVATE quadeplus_backtest_accounting)
target_compile_options(backtest_accounting_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME backtest_accounting COMMAND backtest_accounting_tests
                                          "${CMAKE_CURRENT_BINARY_DIR}/accounting-scratch")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME backtest_accounting_oracle
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_reference.py" --executable
        "$<TARGET_FILE:backtest_accounting_tests>" --scratch
        "${CMAKE_CURRENT_BINARY_DIR}/accounting-oracle")

if(APPLE AND LEXONGINE_XML_FRONTEND)
    include("${CMAKE_CURRENT_SOURCE_DIR}/host_build.cmake")
    add_library(quadeplus_stochastic STATIC
                "${Quadeplus_SOURCE_DIR}/src/quant/strategies/stochastic.cpp")
    target_link_libraries(quadeplus_stochastic PUBLIC quadeplus_backtest_accounting
                                                      lexongine_quant_components)
    add_library(
        quadeplus_backtest_host STATIC "${Quadeplus_SOURCE_DIR}/src/quant/backtest/host.cpp"
                                       "${Quadeplus_SOURCE_DIR}/src/quant/backtest/risk.cpp")
    target_include_directories(quadeplus_backtest_host
                               PRIVATE "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine")
    target_link_libraries(
        quadeplus_backtest_host PUBLIC quadeplus_backtest_accounting quadeplus_stochastic
                                       lexongine_persistence lexongine_plugin_loader)
    target_compile_options(quadeplus_backtest_host PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_library(quadeplus_keltner_features STATIC
                "${Quadeplus_SOURCE_DIR}/src/quant/strategies/features.cpp")
    target_link_libraries(quadeplus_keltner_features PUBLIC quadeplus_backtest_accounting
                                                            lexongine_quant_components)
    add_executable(backtest_features_tests
                   "${Quadeplus_SOURCE_DIR}/tests/backtest_features_tests.cpp")
    target_link_libraries(backtest_features_tests PRIVATE quadeplus_keltner_features)
    add_test(NAME backtest_features
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_features.py"
                     "$<TARGET_FILE:backtest_features_tests>")
    add_library(quadeplus_backtest_reports STATIC
                "${Quadeplus_SOURCE_DIR}/src/quant/backtest/report.cpp")
    target_link_libraries(quadeplus_backtest_reports PUBLIC Quadeplus::core)
    target_compile_options(quadeplus_backtest_reports PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_library(
        quadeplus_backtest_sma STATIC
        "${Quadeplus_SOURCE_DIR}/src/quant/strategies/policies.cpp"
        application.cpp
        keltner_application.cpp
        stochastic_application.cpp
        report_evidence.cpp
        suite.cpp
        suite_publication.cpp)
    target_include_directories(quadeplus_backtest_sma
                               PRIVATE "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine")
    target_link_libraries(
        quadeplus_backtest_sma
        PUBLIC quadeplus_backtest_host quadeplus_keltner_features quadeplus_backtest_reports
        PRIVATE lexongine_frontend lexongine_file_evidence tinyxml2::tinyxml2)
    target_compile_options(quadeplus_backtest_sma PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_executable(backtest_sma_tests "${Quadeplus_SOURCE_DIR}/tests/backtest_sma_tests.cpp")
    target_link_libraries(backtest_sma_tests PRIVATE quadeplus_backtest_sma)
    add_executable(quadeplus_backtest main.cpp host_identity.cpp)
    target_link_libraries(quadeplus_backtest PRIVATE quadeplus_backtest_sma)
    target_compile_options(quadeplus_backtest PRIVATE -Wall -Wextra -Wpedantic -Werror)
    quadeplus_bind_financial_host(quadeplus_backtest)
    add_test(
        NAME backtest_sma
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_sma.py" --runner
            "$<TARGET_FILE:quadeplus_backtest>" --fixture "$<TARGET_FILE:backtest_sma_tests>"
            --config "${Quadeplus_SOURCE_DIR}/examples/backtest/sma.xml" --scratch
            "${CMAKE_CURRENT_BINARY_DIR}/sma-scratch")
    add_executable(backtest_report_tests "${Quadeplus_SOURCE_DIR}/tests/backtest_report_tests.cpp")
    target_link_libraries(backtest_report_tests PRIVATE quadeplus_backtest_reports)
    add_test(NAME backtest_report COMMAND backtest_report_tests)
    add_test(
        NAME backtest_report_oracle
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "BACKTEST_REPORT_TEST_BINARY=$<TARGET_FILE:backtest_report_tests>"
            "${Python3_EXECUTABLE}" -B -m unittest integration.backtest.test_report_metrics -v)
    set_tests_properties(backtest_report_oracle PROPERTIES WORKING_DIRECTORY
                                                           "${Quadeplus_SOURCE_DIR}")
    add_test(NAME backtest_suite_schema
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_suite.py"
                     --self-test)
    add_executable(backtest_suite_publication_tests
                   "${Quadeplus_SOURCE_DIR}/tests/backtest_suite_publication_tests.cpp")
    target_include_directories(
        backtest_suite_publication_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine")
    target_link_libraries(backtest_suite_publication_tests PRIVATE quadeplus_backtest_sma)
    add_test(NAME backtest_suite_publication COMMAND backtest_suite_publication_tests
                                                     "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(
        NAME backtest_sharing
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_sharing.py" --runner
            "$<TARGET_FILE:quadeplus_backtest>" --fixture "$<TARGET_FILE:backtest_sma_tests>"
            --sma-config "${Quadeplus_SOURCE_DIR}/examples/backtest/sma.xml" --stochastic-config
            "${Quadeplus_SOURCE_DIR}/examples/backtest/stochastic-fresh.xml" --scratch
            "${CMAKE_CURRENT_BINARY_DIR}/sharing-scratch")
    add_executable(backtest_keltner_tests
                   "${Quadeplus_SOURCE_DIR}/tests/backtest_keltner_tests.cpp")
    target_link_libraries(backtest_keltner_tests PRIVATE quadeplus_backtest_sma)
    add_test(
        NAME backtest_keltner
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_keltner.py" --runner
            "$<TARGET_FILE:quadeplus_backtest>" --fixture "$<TARGET_FILE:backtest_keltner_tests>"
            --config "${Quadeplus_SOURCE_DIR}/examples/backtest/keltner.xml" --scratch
            "${CMAKE_CURRENT_BINARY_DIR}/keltner-scratch")
    add_executable(backtest_stochastic_tests
                   "${Quadeplus_SOURCE_DIR}/tests/backtest_stochastic_tests.cpp")
    target_link_libraries(backtest_stochastic_tests PRIVATE quadeplus_stochastic)
    add_test(
        NAME backtest_stochastic
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stochastic_test_driver.py"
            "$<TARGET_FILE:backtest_stochastic_tests>"
            "${CMAKE_CURRENT_BINARY_DIR}/stochastic-scratch")
    add_executable(
        backtest_stochastic_host_tests
        "${Quadeplus_SOURCE_DIR}/tests/backtest_stochastic_host_tests.cpp" host_identity.cpp)
    target_include_directories(
        backtest_stochastic_host_tests
        PRIVATE "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine"
                "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(backtest_stochastic_host_tests
                          PRIVATE quadeplus_backtest_sma lexongine_frontend lexongine_file_evidence)
    quadeplus_bind_financial_host(backtest_stochastic_host_tests)
    add_test(
        NAME backtest_stochastic_host
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stochastic_host_test_driver.py"
            "$<TARGET_FILE:backtest_stochastic_host_tests>"
            "${CMAKE_CURRENT_BINARY_DIR}/stochastic-host-scratch")
    add_executable(
        backtest_host_tests
        "${Quadeplus_SOURCE_DIR}/tests/backtest_host_tests.cpp"
        "${Quadeplus_SOURCE_DIR}/tests/backtest_host_support.hpp" host_identity.cpp)
    target_include_directories(
        backtest_host_tests PRIVATE "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine"
                                    "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(backtest_host_tests PRIVATE quadeplus_backtest_host lexongine_frontend
                                                      lexongine_file_evidence)
    target_compile_options(backtest_host_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    quadeplus_bind_financial_host(backtest_host_tests)
    add_test(NAME backtest_host COMMAND backtest_host_tests
                                        "${CMAKE_CURRENT_BINARY_DIR}/host-scratch")
    add_executable(
        backtest_acceptance_tests
        "${Quadeplus_SOURCE_DIR}/tests/backtest_acceptance_tests.cpp"
        "${Quadeplus_SOURCE_DIR}/tests/backtest_host_support.hpp" host_identity.cpp)
    target_include_directories(
        backtest_acceptance_tests PRIVATE "${QUADEPLUS_LEXONGINE_SOURCE_DIR}/integration/lexongine"
                                          "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(backtest_acceptance_tests
                          PRIVATE quadeplus_backtest_sma lexongine_frontend lexongine_file_evidence)
    target_compile_options(backtest_acceptance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    quadeplus_bind_financial_host(backtest_acceptance_tests)
    add_test(NAME backtest_acceptance COMMAND backtest_acceptance_tests
                                              "${CMAKE_CURRENT_BINARY_DIR}/acceptance-scratch")
    add_test(
        NAME backtest_host_build
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_host_build.py" --lexongine
            "${QUADEPLUS_LEXONGINE_SOURCE_DIR}" --scratch
            "${CMAKE_CURRENT_BINARY_DIR}/host-build-scratch")
endif()
