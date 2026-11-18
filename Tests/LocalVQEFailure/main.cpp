// Only the test copy of localvqe_graph.cpp redirects its GGML compute call here.
#undef ggml_backend_graph_compute
#include "ggml-backend.h"
#include "localvqe_api.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

static bool failNext = false;
extern "C" ggml_status mv_test_graph_compute(ggml_backend_t backend, ggml_cgraph *graph) {
    if (failNext) { failNext = false; return GGML_STATUS_ABORTED; }
    return ggml_backend_graph_compute(backend, graph);
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    auto threadCount = reinterpret_cast<unsigned long (*)()>(dlsym(RTLD_DEFAULT, "mv_test_thread_count"));
    if (!threadCount) { std::fputs("Missing test thread counter\n", stderr); return 1; }
    const auto beforeCreate = threadCount();
    const auto ctx = localvqe_new(argv[1]);
    if (!ctx) return 1;
    std::array<float, 256> input{}, far{}, expected{}, actual{};
    input.fill(.2f);
    auto process = [&](auto &out) {
        return localvqe_process_frame_f32(ctx, input.data(), far.data(), input.size(), out.data());
    };
    bool ok = process(expected) == 0;
    const auto warmed = threadCount();
    ok = ok && warmed > beforeCreate; // Positive control for the interposition hook.
    for (int i = 0; i < 100 && ok; ++i) ok = process(actual) == 0;
    ok = ok && threadCount() == warmed;
    if (ok) std::puts("PASS zero new worker threads across 100 warm LocalVQE hops");
    actual.fill(1);
    failNext = true;
    ok = ok && process(actual) != 0 && !failNext && std::strlen(localvqe_last_error(ctx)) > 0
            && std::all_of(actual.begin(), actual.end(), [](float x) { return x == 0; });
    ok = ok && process(actual) == 0 && actual == expected;
    std::vector<float> clip(1024, .2f), reference(1024), output(1024, 1);
    failNext = true;
    ok = ok && localvqe_process_f32(ctx, clip.data(), reference.data(), clip.size(), output.data()) != 0
            && !failNext && std::all_of(output.begin(), output.end(), [](float x) { return x == 0; });
    localvqe_free(ctx);
    if (ok) std::puts("PASS GGML failure propagation, silent output and fresh state on retry");
    return ok ? 0 : 1;
}
