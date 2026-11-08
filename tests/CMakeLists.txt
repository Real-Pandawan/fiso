add_executable(fiso-tests test_main.cpp test_cd.cpp)
target_link_libraries(fiso-tests PRIVATE fiso fiso_flac)
target_include_directories(fiso-tests PRIVATE ${CMAKE_SOURCE_DIR}/libfiso/src)
add_test(NAME fiso-tests COMMAND fiso-tests ${CMAKE_CURRENT_BINARY_DIR}/work)
