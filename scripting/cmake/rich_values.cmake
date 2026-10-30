# NOT ORIGINAL - rich by-value QB codec (issue #21).
# Adds the codec sources to the scripting library and registers its own
# QuickJS test target. Does not change existing target policies.
target_sources(thug2_js PRIVATE
  src/value_codec.cpp)
target_include_directories(thug2_js PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(BUILD_TESTING)
  add_executable(qb-js-value-codec-tests tests/value_codec_tests.cpp)
  target_link_libraries(qb-js-value-codec-tests PRIVATE thug2_js quickjs_core)
  target_compile_features(qb-js-value-codec-tests PRIVATE cxx_std_20)
  target_compile_options(qb-js-value-codec-tests PRIVATE -Wall -Wextra)
  add_test(NAME qb-js-value-codec COMMAND qb-js-value-codec-tests)
  set_tests_properties(qb-js-value-codec PROPERTIES TIMEOUT 120)
endif()
