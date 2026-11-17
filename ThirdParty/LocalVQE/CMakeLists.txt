cmake_minimum_required(VERSION 3.20)
project(MetalVoiceLocalVQE LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
set(GGML_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(upstream/ggml/vendor/ggml ggml)

# Joint v1.3 only. The unused GTCRN backend needs an upstream GRU patch;
# excluding that architecture leaves the vendored GGML sources unchanged.
set(vqe ${CMAKE_CURRENT_SOURCE_DIR}/upstream/ggml)
add_library(mv_localvqe_api STATIC
    ${vqe}/localvqe_api.cpp ${vqe}/localvqe_graph.cpp ${vqe}/model_hash.cpp
    ${vqe}/common.cpp ${vqe}/daf_frontend.cpp)
target_include_directories(mv_localvqe_api PUBLIC ${vqe})
target_link_libraries(mv_localvqe_api PUBLIC ggml)
add_executable(localvqe-regression ${vqe}/tests/test_regression.cpp)
target_link_libraries(localvqe-regression PRIVATE mv_localvqe_api)
