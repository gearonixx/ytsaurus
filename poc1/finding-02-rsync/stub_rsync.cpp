// Stub `rsync` that just prints argv. Used by F02 reproducer.
// Build:  g++ -O2 -o stub_rsync stub_rsync.cpp
// Install: cp stub_rsync ./fakebin/rsync && PATH=$PWD/fakebin:$PATH

#include <stdio.h>

int main(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
        fprintf(stderr, "[stub-rsync] argv[%d] = %s\n", i, argv[i]);
    }
    return 0;
}
