cmake_minimum_required ( VERSION 2.8.5 )
cmake_policy ( SET CMP0054 NEW )
project ( R2T2 )

add_subdirectory ( lib/pbrt )

include ( etc/build_defs.cmake )
include ( etc/build_type.cmake )
include ( etc/cflags.cmake )

include ( etc/clang_format.cmake )
include ( etc/clang_tidy.cmake )

include ( FindProtobuf )
find_package ( Protobuf REQUIRED )
find_library ( PTHREAD pthread )

include ( FindOpenSSL )

include ( etc/find_lz4.cmake )

include_directories ( SYSTEM
  "${CMAKE_CURRENT_LIST_DIR}/lib/pbrt/src/include"
  "${CMAKE_CURRENT_LIST_DIR}/lib/pbrt/src/core"
  "${CMAKE_CURRENT_LIST_DIR}/lib/pbrt/src/ext/glog/src"
  "${CMAKE_CURRENT_LIST_DIR}/lib"
  "${CMAKE_CURRENT_BINARY_DIR}/lib/pbrt/src/ext/glog"
)

add_subdirectory ( src/messages )
include_directories ( "${CMAKE_CURRENT_BINARY_DIR}/src/messages" )

add_definitions ( -D PBRT_CONSTEXPR=constexpr )
add_definitions ( -D PBRT_THREAD_LOCAL=thread_local )

file ( GLOB R2T2_SOURCE
    #src/benchmark/*
    src/common/*
    src/master/*
    src/messages/*
    src/net/*
    src/remote/*
    src/schedulers/*
    #src/server/*
    src/simulator/*
    src/storage/*
    src/util/*
    src/worker/*
)

include_directories ( src )

add_library( r2t2 STATIC
    ${R2T2_SOURCE}
)

set ( ALL_R2T2_LIBS
    r2t2
    r2t2_messages
    pbrt
    pbrt_messages
    atomic
    ${CMAKE_THREAD_LIBS_INIT}
    IlmImf
    Imath
    Half
    glog
    Ptex_static
    ${LZ4_LIBRARY}
    ${ZLIB_LIBRARY}
    ${Protobuf_LIBRARIES}
    ${OPENSSL_LIBRARIES}
)

add_executable ( r2t2-lambda-master src/frontend/lambda-master.cc )
target_link_libraries( r2t2-lambda-master ${ALL_R2T2_LIBS} )

set ( WORKER_LINK_FLAGS dl z unwind lzma -static -Wl,-allow-multiple-definition
                        -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
                        -s )

if ( CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES ASan )
  set( WORKER_LINK_FLAGS "" )
endif()

add_executable ( r2t2-lambda-worker src/frontend/lambda-worker.cc )
target_link_libraries( r2t2-lambda-worker ${ALL_R2T2_LIBS} ${WORKER_LINK_FLAGS} )

add_executable ( r2t2-aggregate src/frontend/aggregate.cc )
target_link_libraries( r2t2-aggregate ${ALL_R2T2_LIBS} )

add_executable ( aggregate-timelapse src/frontend/aggregate-timelapse.cc )
target_link_libraries( aggregate-timelapse ${ALL_R2T2_LIBS} )

add_executable ( r2t2-simulator src/frontend/simulator.cc )
target_link_libraries( r2t2-simulator ${ALL_R2T2_LIBS} )

add_executable ( load-treelet src/frontend/load-treelet.cc )
target_link_libraries( load-treelet ${ALL_R2T2_LIBS} )

add_executable ( camera-generator src/frontend/camera-generator.cc )
target_link_libraries( camera-generator ${ALL_R2T2_LIBS} )

add_executable ( path-tracer src/frontend/path-tracer.cc )
target_link_libraries( path-tracer ${ALL_R2T2_LIBS} )

add_executable ( treelet-tracer src/frontend/treelet-tracer.cc )
target_link_libraries( treelet-tracer ${ALL_R2T2_LIBS} )
