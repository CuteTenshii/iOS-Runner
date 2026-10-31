#include <malloc.h>

#include <cstdio>
#include <cstdlib>

#include "engine.h"

// Static Bionic aborts at startup unless the TLS segment is 64-byte aligned.
alignas(64) thread_local volatile char g_tls_alignment[64];

int main(int argc, char** argv) {
    g_tls_alignment[0] = 0;
    // Matches the manifest's allowNativeHeapPointerTagging="false": tags collide with Swift's spare bits.
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
    if (argc != 3) {
        fprintf(stderr, "usage: %s <bundle dir> <executable name>\n", argv[0]);
        return 2;
    }
    // Without a UI there is nothing to act: print the first tree and let the guest finish.
    engine_set_message_sink(+[](const char* kind, const char* payload) { fprintf(stdout, "EMIT %s %s\n", kind, payload); fflush(stdout); });
    engine_close_actions();
    return run_app_bundle(argv[1], argv[2], "");
}
