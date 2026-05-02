// F02 — rsync argv-injection reproducer (non-weaponized).
//
// This program models the vulnerable execl call in
// yt/yt/server/tools/proc.cpp:236 and demonstrates that an attacker-controlled
// `Source` value beginning with `-` is delivered to rsync as an OPTION rather
// than a path. We do NOT run real rsync; we run a tiny stub `rsync` that
// prints argv and exits. Build and use:
//
//   $ g++ -O2 -o repro repro.cpp
//   $ g++ -O2 -o stub_rsync stub_rsync.cpp
//   $ mkdir -p ./fakebin && cp stub_rsync ./fakebin/rsync
//   $ PATH=$PWD/fakebin:$PATH ./repro
//
// Expected output: the attacker-controlled value `--version` appears in the
// argv slot that rsync would parse as an option, NOT as a positional path.
//
// No network, no filesystem changes, no privilege escalation. The point is
// vendor verification of the code path.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>

// Mirrors the vulnerable execl pattern.
static void VulnerableSpawn(const std::string& source, const std::string& dest) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rsync", "rsync", "-q", "--perms", "--recursive", "--specials",
               "--links", source.c_str(), dest.c_str(), (char*)nullptr);
        perror("execlp");
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

int main() {
    fprintf(stderr,
        "[*] F02 reproducer — modelling proc.cpp:236 with a stub rsync.\n"
        "[*] Attacker-controlled config:\n"
        "[*]   source      = \"--version\"   (benign payload)\n"
        "[*]   destination = \"/tmp/dest\"\n\n");

    // Benign attacker-supplied values.
    std::string source = "--version";
    std::string dest   = "/tmp/dest";

    VulnerableSpawn(source, dest);

    fprintf(stderr,
        "\n[*] If the stub printed `--version` in an argv slot, rsync would\n"
        "    have parsed it as an OPTION rather than a path. In production,\n"
        "    swapping `--version` for `--rsh=/path/cmd` (or other rsync flags)\n"
        "    achieves command execution under the privileges of the caller —\n"
        "    which is uid 0 in TCopyDirectoryContentTool.\n");
    return 0;
}
