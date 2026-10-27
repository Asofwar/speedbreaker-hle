// Tests for the game folder checks startup makes before any of the game runs
// (install/installer.h, from main.cpp): CheckInstalledFiles, every manifest
// file there by stat alone (found in any case, as the runtime opens files),
// with the manifest's size; and CheckInstalledXex, the default.xex startup
// read being the supported one. GitHub issue #4: a game folder copied part
// way started, then failed minutes in at the first file it lacked. Also
// CheckGameVersion, whose messages CheckInstalledXex now shares. On a small
// manifest, in a new folder made inside [folder] (default: the current
// folder), deleted afterwards. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/startup_game_files_test.cpp runtime/install/{placement,installer,locate,manifest,result,disc_source,sha256}.cpp runtime/user/portable.cpp -o build/startup_game_files_test
//   (on macOS add -framework CoreFoundation)
//   build/startup_game_files_test [folder]
#include <install/installer.h>
#include <install/sha256.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <unistd.h>

using namespace install;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool Has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

static void Write(const fs::path& file, size_t size)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << std::string(size, 'x');
}

// default.xex's bytes in the folders below (not an XEX: no title ID).
static const std::string kXex(16, 'x');

// A disc of four files in two folders, as the manifest spells them.
static const ManifestFile kFiles[] = {
    { "default.xex", 16, "" },  // its hash is filled in by main()
    { "NFS/GLOBAL/GLOBALA.BUN", 5, "00" },
    { "NFS/TRACKS/STREAML2RA.BUN", 7, "00" },
    { "Movies/intro.vp6", 3, "00" },
};

static void MakeFolder(const fs::path& dir)
{
    fs::remove_all(dir);
    for (const ManifestFile& f : kFiles)
        Write(dir / f.path, f.size);
}

static void Files(const fs::path& root, const Manifest& manifest)
{
    printf("files\n");
    fs::path dir = root / "game";

    MakeFolder(dir);
    Result r = CheckInstalledFiles(dir, manifest);
    CHECK(r.Ok(), "a complete folder: %s", r.message.c_str());

    // Files nobody listed (dlc/, the player's own) don't matter.
    Write(dir / "dlc/extra.bin", 9);
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.Ok(), "a complete folder with more in it: %s", r.message.c_str());

    // Names in another case (a tool that upper- or lower-cased them, on a
    // case-sensitive file system): found, as the runtime finds them.
    fs::rename(dir / "NFS", dir / "nfs");
    fs::rename(dir / "nfs/TRACKS/STREAML2RA.BUN", dir / "nfs/TRACKS/streaml2ra.bun");
    fs::rename(dir / "Movies", dir / "MOVIES");
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.Ok(), "names in another case: %s", r.message.c_str());

    // One file missing: named, as the folder's.
    MakeFolder(dir);
    fs::remove(dir / "NFS/TRACKS/STREAML2RA.BUN");
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::MissingFile && r.file == "NFS/TRACKS/STREAML2RA.BUN", "one missing: %s (%s)", ErrorName(r.error),
        r.file.c_str());
    CHECK(Has(r.message, "is incomplete") && Has(r.message, "NFS/TRACKS/STREAML2RA.BUN is missing.") &&
        Has(r.message, dir.string().c_str()) && !Has(r.message, "(and "), "one missing: %s", r.message.c_str());

    // A whole folder missing (the copy stopped part way): the first file
    // named, the rest counted.
    MakeFolder(dir);
    fs::remove_all(dir / "NFS");
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::MissingFile && Has(r.message, "NFS/GLOBAL/GLOBALA.BUN is missing (and 1 more)."), "a folder missing: %s",
        r.message.c_str());

    // A file cut short: its size and the manifest's.
    MakeFolder(dir);
    Write(dir / "NFS/GLOBAL/GLOBALA.BUN", 2);
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::CorruptFile && r.file == "NFS/GLOBAL/GLOBALA.BUN" &&
        Has(r.message, "is damaged: NFS/GLOBAL/GLOBALA.BUN is 2 bytes, but should be 5."), "one cut short: %s", r.message.c_str());

    // Missing goes first, whatever else is wrong.
    fs::remove(dir / "Movies/intro.vp6");
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::MissingFile && r.file == "Movies/intro.vp6", "missing and cut short: %s", r.message.c_str());

    // A folder by a file's name is no file.
    MakeFolder(dir);
    fs::remove(dir / "Movies/intro.vp6");
    fs::create_directories(dir / "Movies/intro.vp6");
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::MissingFile && r.file == "Movies/intro.vp6", "a folder for a file: %s", r.message.c_str());

    // No folder at all.
    fs::remove_all(dir);
    r = CheckInstalledFiles(dir, manifest);
    CHECK(r.error == Error::NotFound && Has(r.message, "isn't there"), "no folder: %s (%s)", ErrorName(r.error), r.message.c_str());
}

static void Xex(const fs::path& root, const Manifest& manifest)
{
    printf("default.xex\n");
    fs::path dir = root / "game";
    const std::string good(manifest.Xex().sha256);
    const std::string other = std::string(64, 'a');

    Result r = CheckInstalledXex(dir, good, manifest.titleId, 16, manifest);
    CHECK(r.Ok(), "the supported default.xex: %s", r.message.c_str());

    // The same game, another version: the hash it has and the one it needs,
    // and (the same size) maybe a damaged dump.
    r = CheckInstalledXex(dir, other, manifest.titleId, 16, manifest);
    CHECK(r.error == Error::UnsupportedVersion && r.foundHash == other, "another version: %s", ErrorName(r.error));
    CHECK(Has(r.message, ("The game folder " + dir.string() + " is Need for Speed: Most Wanted, but not a version this build supports").c_str()) &&
        Has(r.message, other.c_str()) && Has(r.message, good.c_str()) && Has(r.message, "unless this one is a damaged dump"),
        "another version: %s", r.message.c_str());
    r = CheckInstalledXex(dir, other, manifest.titleId, 17, manifest);
    CHECK(r.error == Error::UnsupportedVersion && !Has(r.message, "damaged"), "another version, another size: %s", r.message.c_str());

    // Another game, or no Xbox 360 executable at all.
    r = CheckInstalledXex(dir, other, 0x4D5307E6, 16, manifest);
    CHECK(r.error == Error::WrongGame && Has(r.message, "holds another game (title ID 4D5307E6"), "another game: %s", r.message.c_str());
    r = CheckInstalledXex(dir, other, 0, 16, manifest);
    CHECK(r.error == Error::WrongGame && Has(r.message, "not an Xbox 360 game executable"), "no title ID: %s", r.message.c_str());

    // CheckGameVersion (a source's default.xex) says the same of "This
    // folder": its messages are CheckInstalledXex's.
    MakeFolder(dir);
    std::unique_ptr<DiscSource> source;
    r = OpenDiscSource(dir, source);
    CHECK(r.Ok() && source, "opening the folder: %s", r.message.c_str());
    if (source)
    {
        r = CheckGameVersion(*source, manifest);
        CHECK(r.Ok(), "the supported default.xex in a source: %s", r.message.c_str());
        static const ManifestFile otherFiles[] = { { "default.xex", 16, "bb" } };
        Manifest otherManifest{ "test-other", manifest.titleId, otherFiles, 16 };
        r = CheckGameVersion(*source, otherManifest);
        CHECK(r.error == Error::WrongGame && Has(r.message, "This folder is not Need for Speed: Most Wanted: its default.xex is not") &&
            r.foundHash == good, "another default.xex in a source: %s (%s)", ErrorName(r.error), r.message.c_str());
    }
}

int main(int argc, char** argv)
{
    // default.xex's hash: what the folders' 16 bytes hash to.
    static const std::string xexHash = ToHex(Sha256Of(kXex.data(), kXex.size()));
    static ManifestFile files[std::size(kFiles)];
    uint64_t total = 0;
    for (size_t i = 0; i < std::size(kFiles); i++)
    {
        files[i] = kFiles[i];
        total += files[i].size;
    }
    files[0].sha256 = xexHash;
    Manifest manifest{ "test-1", 0x454107D9, files, total };

    fs::path parent = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    std::string pattern = (parent / "startup_game_files_XXXXXX").string();
    if (!mkdtemp(pattern.data()))
    {
        perror("mkdtemp");
        return 1;
    }
    fs::path root = pattern;

    Files(root, manifest);
    Xex(root, manifest);

    fs::remove_all(root, ec);
    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
