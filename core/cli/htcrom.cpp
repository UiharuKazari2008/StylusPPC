// htcrom: the app's HTC ROM import (core/src/htc/romkit.h) from a shell, for tests on the phone.
//   htcrom <rom.nbh|os.nb|imgfs.bin> list [substring]
//   htcrom <rom> stage <package-name-or-prefix> <out-dir> [windows.txt]
// windows.txt: the guest's \Windows DLL names, one per line (the helper writes htc\windows.txt).
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>

#include "htc/romkit.h"

static std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: htcrom <rom> list [substring] | stage <package> <out-dir> [windows.txt]\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    auto t0 = std::chrono::steady_clock::now();
    htc::Rom rom;
    std::string err;
    if (!rom.open(fd, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "opened in %.0f ms, %zu packages\n", ms, rom.packages().size());
    std::string cmd = argv[2];
    if (cmd == "list") {
        std::string pat = argc > 3 ? lower(argv[3]) : "";
        for (auto &p : rom.packages()) {
            std::string exes;
            for (auto &f : p.files) if (lower(f).size() > 4 && lower(f).compare(f.size() - 4, 4, ".exe") == 0) exes += " " + f;
            if (!pat.empty() && lower(p.name + exes).find(pat) == std::string::npos) continue;
            printf("%s\t%zu files\t%s\n", p.name.c_str(), p.files.size(), exes.c_str());
        }
        return 0;
    }
    if (cmd == "stage" && argc >= 5) {
        std::set<std::string> have;
        if (argc > 5) {
            std::ifstream in(argv[5]);
            for (std::string l; std::getline(in, l);) {
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (!l.empty() && l[0] != '#') have.insert(lower(l));
            }
        }
        std::string log;
        bool ok = rom.stage(argv[3], argv[4], argc > 5 ? &have : nullptr, &log);
        fputs(log.c_str(), stdout);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "done in %.0f ms\n", ms);
        return ok ? 0 : 1;
    }
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
