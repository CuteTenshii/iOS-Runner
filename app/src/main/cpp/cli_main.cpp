#include <cstdio>
#include <string>

#include "engine.h"

// Static Bionic aborts at startup unless the TLS segment is 64-byte aligned.
alignas(64) thread_local volatile char g_tls_alignment[64];

int main(int argc, char** argv) {
    g_tls_alignment[0] = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: %s <bundle dir> <executable name>\n", argv[0]);
        return 2;
    }
    return run_app_bundle(argv[1], argv[2], "");
}
