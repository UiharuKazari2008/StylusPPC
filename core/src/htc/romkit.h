// HTC ROM kit: installs apps from an HTC HD2 ROM the user owns into a PocketStylus guest.
//
// Reads an HD2 RUU_signed.nbh (or an os.nb dump, or a raw IMGFS partition), lists its ROM packages
// and stages one package as a folder the guest helper installs ("htcinstall", guest/Helper/HtcCompat.cs).
// The on-device form of tools/hd2/nbh2nb.py + imgfs.py + dsm.py + stage.py (formats in
// docs/research-hd2-port.md). Staged folder:
//   <files>          the package's files; ROM modules rebuilt into PE files with recovered relocations
//   deps/<dll>       ROM DLLs the package imports that the guest lacks (installed only if missing)
//   <package>.reg    the package's registry (.rgu, UTF-16) as UTF-8 REGEDIT4
//   install.ini      written last: package=, file=, dep=, reg=, link=<shortcut>|<target>, missing=
// Imports of DDRAW.dll are renamed to HDRAW.dll (the DirectDraw caps shim, guest/DdShim) and the
// Authenticode signature of a changed binary is dropped (CE refuses a bad signature, runs unsigned).
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace htc {

struct Package {
    std::string name;                // e.g. Teeter_2_2_20203823_p0
    std::vector<std::string> files;  // as the .dsm lists them
};

class Source;
class Imgfs;

class Rom {
public:
    Rom();
    ~Rom();
    /** Opens [fd] (kept open by the caller; read with pread). False with [err] set if not a ROM. */
    bool open(int fd, std::string *err);
    const std::vector<Package> &packages() const { return packages_; }
    /**
     * Stages the package named [name] (exact, or a unique case-insensitive prefix) into [outDir]
     * (created). [have]: lowercase DLL names already in the guest's \Windows (the helper writes
     * htc\windows.txt); with it, ROM dependencies are followed transitively, without it only the
     * package's own imports are staged (the guest still skips those it has). Log lines go to [log].
     */
    bool stage(const std::string &name, const std::string &outDir, const std::set<std::string> *have, std::string *log);

private:
    std::unique_ptr<Source> file_, nbh_, nand_, part_;
    std::unique_ptr<Imgfs> fs_;
    std::vector<Package> packages_;
};

}  // namespace htc
