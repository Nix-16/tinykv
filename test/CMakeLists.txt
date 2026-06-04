# 单元/集成测试，统一通过 CTest 运行。
# 每个测试是独立可执行文件，链接 tinykv_core。

set(TINYKV_TESTS
    test_buffer
    test_store
    test_resp
    test_persistence
    test_database
)

foreach(t ${TINYKV_TESTS})
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE tinykv_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${t} COMMAND ${t})
endforeach()
