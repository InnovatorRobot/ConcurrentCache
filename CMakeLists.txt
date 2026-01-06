cmake_minimum_required(VERSION 3.14)
project(ThreadSafeTtlLruCache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(lru_ttl_cache INTERFACE)
target_include_directories(lru_ttl_cache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS lru_ttl_cache
    EXPORT lru_ttl_cacheTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(FILES include/lru_ttl_cache_thread_safe.hpp
    DESTINATION include
)

install(EXPORT lru_ttl_cacheTargets
    FILE lru_ttl_cacheTargets.cmake
    NAMESPACE lru_ttl_cache::
    DESTINATION lib/cmake/lru_ttl_cache
)

if(BUILD_TESTS)
    enable_testing()
    
    # Find or download Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    
    add_executable(test_lru_ttl_cache
        tests/test_lru_ttl_cache.cpp
    )
    
    target_link_libraries(test_lru_ttl_cache
        PRIVATE
        lru_ttl_cache
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
    )
    
    add_test(NAME LruTtlCacheTests COMMAND test_lru_ttl_cache)
endif()

