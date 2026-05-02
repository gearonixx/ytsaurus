// F04 — rm argv-injection reproducer (non-weaponized).
//
// Models proc.cpp:52 with a stub rm that just prints argv. No files are
// removed. Build with the stub from finding-02-rsync/stub_rsync.cpp.
//
//   g++ -O2 -o repro repro.cpp
//   mkdir -p ./fakebin
//   g++ -O2 -o ./fakebin/rm ../finding-02-rsync/stub_rsync.cpp
//   PATH=$PWD/fakebin:$PATH ./repro

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>

static void VulnerableSpawn(const std::string& path) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rm", "rm", "-rf", path.c_str(), (char*)nullptr);
        perror("execlp");
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

int main() {
    fprintf(stderr,
        "[*] F04 reproducer — modelling proc.cpp:52 with a stub rm.\n"
        "[*] Attacker-controlled path = \"--help\" (benign payload).\n\n");

    VulnerableSpawn("--help");

    fprintf(stderr,
        "\n[*] In production, replacing `--help` with `--no-preserve-root`\n"
        "    plus a follow-up call with `/` removes the entire filesystem.\n"
        "    Defence: insert `--` in the execl call and validate path.\n");
    return 0;
}
