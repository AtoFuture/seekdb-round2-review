// Functional ISA-dispatch check only; emulator timings are never benchmarks.
#define main validated_vector_tests_main
#include "../tests/validated_vector_tests.cpp"
#undef main
#include <cstring>
int main(int argc,char** argv) {
#if defined(__x86_64__) && defined(__GNUC__)
    const bool available=__builtin_cpu_supports("avx2");
    std::cout<<"runtime AVX2 available: "<<available<<'\n';
    if(argc!=2 || available!=(std::strcmp(argv[1],"avx2")==0))return 2;
    return validated_vector_tests_main();
#else
    (void)argc;(void)argv;return 3;
#endif
}
