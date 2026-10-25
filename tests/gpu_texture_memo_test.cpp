// Checks for the rules the renderer's texture lookups are kept by
// (gpu/renderer.cpp: TextureCacheEntry, TextureDrawMemo). A draw's textures
// are served again without being looked up (CP_OPT_TEXTURE_MEMO) only while
// every input of those lookups is what it was: the layout, the fetch
// constants they read, the write epoch, the cache's generation, whether the
// draw renders scaled, the frame (each texture's lastUsed) and no eviction
// since. These live in renderer.cpp's Vulkan code, so they are checked in
// its source: each part of the memo's key is compared, the memo is made only
// from kept lookups with nothing moved meanwhile, an eviction forgets it,
// and Draw skips comparing its bound textures only while the lookups'
// results are unchanged. NFSMW_CP_CACHE_VERIFY=1 checks the same at run
// time (every draw the memo serves is looked up anyway and compared).
// From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG tests/gpu_texture_memo_test.cpp -o build/gpu_texture_memo_test
//   build/gpu_texture_memo_test runtime/gpu/renderer.cpp
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The source without spaces, tabs and newlines (formatting can't break a check).
static std::string Squash(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r')
            r += c;
    return r;
}

// The text from `from` to the next `to` after it (empty if either is missing).
static std::string Between(const std::string& all, const std::string& from, const std::string& to)
{
    size_t a = all.find(from);
    if (a == std::string::npos)
        return {};
    size_t b = all.find(to, a + from.size());
    return b == std::string::npos ? std::string() : all.substr(a, b - a);
}

static void TestRenderer(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string all = Squash(ss.str());
    auto count = [&](const std::string& in, const std::string& what) {
        size_t n = 0;
        for (size_t at = in.find(what); at != std::string::npos; at = in.find(what, at + 1))
            n++;
        return n;
    };

    // The memo holds only with every part of its key unchanged: the layout
    // object, the write epoch, the generation, the frame, the scaled path's
    // choice, and the 6 dwords of each fetch constant the layout reads.
    const std::string holds = Between(all, "boolTextureMemoHolds(", "voidLookupTextures(");
    CHECK(!holds.empty(), "TextureMemoHolds, then LookupTextures, not found");
    CHECK(count(holds, "m.layout!=&layout||m.epoch!=epoch||m.generation!=c.generation||m.frame!=s_frame||m.drawScaled!=drawScaled") == 1,
        "TextureMemoHolds compares the layout, epoch, generation, frame and scaled choice");
    CHECK(count(holds, "for(uint32_tbits=layout.fetchSlots;bits;bits&=bits-1)") == 1 &&
            count(holds, "memcmp(m.fetch[n],regs+n*6,sizeof(m.fetch[n]))!=0") == 1,
        "TextureMemoHolds compares every fetch constant the layout reads, all 6 dwords");

    // It is made from one pass of lookups whose results were all kept, with
    // the generation and the evictions unchanged across the pass, and the
    // results' serial moves on at every pass (Draw's bound set relies on it).
    const std::string lookups = Between(all, "voidLookupTextures(", "//Resolves");
    CHECK(!lookups.empty(), "LookupTextures not found");
    CHECK(count(lookups, "c.memo.layout=nullptr;boolkept=c.memoOn&&!(s_exp&8);") == 1, "LookupTextures forgets the memo before it looks up");
    CHECK(count(lookups, "kept=kept&&k;") == 1, "every binding must be a kept lookup");
    CHECK(count(lookups, "c.infosSerial++;") == 1, "every pass of lookups moves the results' serial on");
    CHECK(count(lookups, "if(!kept||c.generation!=generation||c.evictions!=evictions)return;") == 1,
        "the memo is made only when the generation and evictions didn't move during the lookups");
    CHECK(count(lookups, "memcpy(m.fetch[n],regs+n*6,sizeof(m.fetch[n]));") == 1 && lookups.rfind("m.layout=&layout;") != std::string::npos,
        "the memo copies the fetch constants it was looked up with, and holds the layout last");

    // GetTexture says "kept" only for an entry's result.
    const std::string get = Between(all, "VkImageViewGetTexture(", "boolTextureMemoHolds(");
    CHECK(count(get, "kept=true;") == 3, "kept at a hit, a verified hit and a kept miss (%zu)", count(get, "kept=true;"));
    CHECK(count(get, "kept=false;") == 1 && get.find("kept=false;") < get.find("if(!c.on)"), "not kept by default (the cache off, a placeholder)");

    // Draw reads the epoch before the memo check and the lookups, and looks
    // its textures up unless the memo holds them.
    CHECK(count(all, "constuint64_tepoch=writewatch::WriteEpoch();constbooldrawScaled=s_drawScaled&&s_scaled->enabled;"
                     "if(!textureCache.memoOn||!TextureMemoHolds(layout,epoch,drawScaled))LookupTextures(layout,epoch,drawScaled);") == 1,
        "Draw reads the epoch first and looks its textures up unless the memo holds them");
    // The bound textures are compared again unless they were last found
    // equal to (or pushed from) these very results; a reset forgets that.
    CHECK(count(all, "if(same&&bound.textureSerial!=textureCache.infosSerial)") == 1 &&
            count(all, "bound.textureSerial=textureCache.infosSerial;") == 1,
        "Draw skips the compare only for the results it last pushed or matched");
    CHECK(count(all, "if(fresh){bound=BoundState{};") == 1, "a reset clears the bound state, its serial included");

    // An eviction forgets the memo; everything that drops textures or changes
    // what a lookup decides moves the generation.
    CHECK(count(all, "c.memo.layout=nullptr;c.evictions++;") == 1, "EvictTextures forgets the memo");
    CHECK(count(all, "s_textureCache->generation++;") >= 3 && count(all, "textureCache.generation++;") >= 1,
        "DropTextures, the scaled memory and the settings move the generation");
}

int main(int argc, char** argv)
{
    if (argc > 1)
        TestRenderer(argv[1]);
    else
        printf("(no renderer.cpp given: nothing checked)\n");
    if (g_failures)
    {
        printf("gpu_texture_memo_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_texture_memo_test: all passed\n");
    return 0;
}
