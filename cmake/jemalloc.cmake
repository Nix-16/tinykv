# jemalloc.cmake —— 从源码编译自带的 jemalloc 子模块
#
# 由 third_party/jemalloc（git submodule，仅源码）在构建阶段执行
#   autogen.sh -> configure --with-jemalloc-prefix=je_ ... -> make build_lib_static
# 产出带 je_ 前缀的静态库 libjemalloc.a，供 operator new/delete 重载使用。
#
# 调用本模块后，若成功会定义 IMPORTED 目标 tinykv::jemalloc，并把
# TINYKV_JEMALLOC_FOUND 置为 ON。子模块缺失（未 init）时不报错，
# 仅打印提示并保持 OFF，让上层回退到系统分配器。

include(ExternalProject)

function(tinykv_setup_jemalloc)
    set(JEMALLOC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jemalloc)

    if(NOT EXISTS ${JEMALLOC_SRC}/autogen.sh)
        message(STATUS
            "jemalloc submodule not found at third_party/jemalloc "
            "(run: git submodule update --init --recursive); "
            "falling back to system allocator")
        set(TINYKV_JEMALLOC_FOUND OFF PARENT_SCOPE)
        return()
    endif()

    set(JEMALLOC_PREFIX  ${CMAKE_CURRENT_BINARY_DIR}/jemalloc)
    set(JEMALLOC_INSTALL ${JEMALLOC_PREFIX}/install)
    set(JEMALLOC_INCLUDE_DIR ${JEMALLOC_INSTALL}/include)
    set(JEMALLOC_STATIC_LIB
        ${JEMALLOC_INSTALL}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jemalloc${CMAKE_STATIC_LIBRARY_SUFFIX})

    # 在源码树外构建（VPATH），避免污染 submodule 工作区。
    ExternalProject_Add(jemalloc_ep
        SOURCE_DIR        ${JEMALLOC_SRC}
        PREFIX            ${JEMALLOC_PREFIX}
        # 源码自带 autogen.sh 生成 configure；只需跑一次。
        CONFIGURE_COMMAND
            ${JEMALLOC_SRC}/autogen.sh
        COMMAND
            ${JEMALLOC_SRC}/configure
                --prefix=${JEMALLOC_INSTALL}
                --with-jemalloc-prefix=je_
                --disable-cxx
                --enable-static
                --disable-shared
        BUILD_COMMAND     ${CMAKE_COMMAND} -E env make build_lib_static -j
        # 仅安装静态库与头文件即可；用 install_lib_static + install_include。
        INSTALL_COMMAND   ${CMAKE_COMMAND} -E env make install_lib_static install_include
        # autogen 必须在源码目录内执行（生成 configure 等到源码树）。
        BUILD_IN_SOURCE   1
        BUILD_BYPRODUCTS  ${JEMALLOC_STATIC_LIB}
        LOG_CONFIGURE     1
        LOG_BUILD         1
        LOG_INSTALL       1
    )

    # 安装阶段才会创建 include 目录；预先建出来以满足 INTERFACE 校验。
    file(MAKE_DIRECTORY ${JEMALLOC_INCLUDE_DIR})

    add_library(tinykv::jemalloc STATIC IMPORTED GLOBAL)
    set_target_properties(tinykv::jemalloc PROPERTIES
        IMPORTED_LOCATION ${JEMALLOC_STATIC_LIB}
        INTERFACE_INCLUDE_DIRECTORIES ${JEMALLOC_INCLUDE_DIR})
    # jemalloc 静态库依赖 dl / pthread。
    target_link_libraries(tinykv::jemalloc INTERFACE dl pthread)
    add_dependencies(tinykv::jemalloc jemalloc_ep)

    set(TINYKV_JEMALLOC_FOUND ON PARENT_SCOPE)
    message(STATUS "jemalloc: will build from source (third_party/jemalloc, prefixed je_)")
endfunction()
