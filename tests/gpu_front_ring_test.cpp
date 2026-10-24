// Tests for gpu/front_ring.h: which of its front images the renderer's next
// swap writes. Up to v0.1.4 one image was written at every swap, and a swap
// between the presenter taking frame N's image and queueing its read of it
// (it waits for a fence and for a swapchain image first, up to a refresh)
// showed N+1 in N's place, then N+1 again, with nothing counted. A model of
// the two threads, driven as presenter.cpp's RunFrame and ClaimFrontImage
// and renderer.cpp's PresentFrontBuffer do, runs scripted and random
// interleavings: with the ring every frame is shown as itself or counted as
// dropped; the model reproduces v0.1.4's race with one image, and a planted
// wrong rule (the presenter's image not avoided) must fail. From the repo
// root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/gpu_front_ring_test.cpp -o build/gpu_front_ring_test
//   build/gpu_front_ring_test runtime/gpu/renderer.cpp runtime/video/presenter.cpp
// With the two paths (ctest passes them), the sources are also checked to
// choose the image under the presenter's lock, before the write is recorded,
// and to mark the presenter's image in the same hold of the lock as the take.
#include <gpu/front_ring.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
static bool g_quiet = false;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; if (!g_quiet) { printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } } while (0)

using Handle = uint64_t;  // a VkImage's stand-in (0: VK_NULL_HANDLE)
using Kind = gpu::FrontChoice::Kind;
using Chooser = std::function<gpu::FrontChoice(const Handle*, int, Handle, Handle)>;

static const char* KindName(Kind k)
{
    return k == Kind::Free ? "free" : k == Kind::TakenBack ? "taken back" : "presenter's";
}

static void Choice(const char* what, std::vector<Handle> images, Handle presenter, Handle waiting, int index, Kind kind)
{
    const gpu::FrontChoice c = gpu::ChooseFrontImage(images.data(), int(images.size()), presenter, waiting);
    CHECK(c.index == index && c.kind == kind, "%s: image %d (%s), want %d (%s)", what, c.index, KindName(c.kind), index, KindName(kind));
}

static void TestChoice()
{
    Choice("nothing held", { 1, 2 }, 0, 0, 0, Kind::Free);
    Choice("the presenter's is the first", { 1, 2 }, 1, 0, 1, Kind::Free);
    Choice("the presenter's is the second", { 1, 2 }, 2, 0, 0, Kind::Free);
    Choice("the waiting frame's is the first", { 1, 2 }, 0, 1, 1, Kind::Free);
    Choice("presenter's and waiting: the waiting one taken back", { 1, 2 }, 1, 2, 1, Kind::TakenBack);
    Choice("the same, the other way round", { 1, 2 }, 2, 1, 0, Kind::TakenBack);
    Choice("another ring's images: all free", { 1, 2 }, 7, 8, 0, Kind::Free);
    Choice("three: the third free", { 1, 2, 3 }, 1, 2, 2, Kind::Free);
    Choice("three: the first free", { 1, 2, 3 }, 3, 2, 0, Kind::Free);
    Choice("one image, nothing held", { 1 }, 0, 0, 0, Kind::Free);
    Choice("one image, waiting", { 1 }, 0, 1, 0, Kind::TakenBack);
    Choice("one image, the presenter's", { 1 }, 1, 0, 0, Kind::Presenter);
    Choice("one image, the presenter's and waiting", { 1 }, 1, 1, 0, Kind::Presenter);
}

// The renderer and the presenter around one ring. Queue order is the order
// of the events: the renderer's write is queued at its swap, the presenter's
// read at Queue, which sees what the last write queued before it left.
struct Model
{
    Chooser choose;
    std::vector<Handle> images;
    std::vector<uint64_t> content;  // by image: the serial last written
    // The presenter's state, as presenter.cpp keeps it under s_pendingMutex.
    Handle pending = 0, last = 0;
    uint64_t pendingSerial = 0, lastSerial = 0, serial = 0, lastOverwritten = 0;
    bool pendingNew = false, lastQueued = false;
    bool holding = false, holdingNew = false;  // between a take (or a redraw's start) and its submit
    // What happened.
    uint64_t dropped = 0, overwritten = 0, shown = 0, wrong = 0, lastShown = 0, outOfOrder = 0;

    Model(Chooser c, int count) : choose(std::move(c))
    {
        for (int i = 0; i < count; i++)
            images.push_back(Handle(i + 1));
        content.assign(count + 1, 0);
    }

    // PresentFrontBuffer: video::ClaimFrontImage, the write, SubmitFrontImage.
    void Swap()
    {
        const gpu::FrontChoice c = choose(images.data(), int(images.size()), last, pending);
        if (c.kind == Kind::TakenBack)
        {
            dropped++;
            pending = 0;
            pendingNew = false;
        }
        else if (c.kind == Kind::Presenter && !lastQueued && lastSerial != lastOverwritten)
        {
            lastOverwritten = lastSerial;
            overwritten++;
            dropped++;
        }
        const Handle image = images[c.index];
        content[image] = ++serial;
        if (pendingNew)
            dropped++;
        pending = image;
        pendingSerial = serial;
        pendingNew = true;
    }

    // RunFrame's take: the waiting frame, or (nothing new) a redraw of the last.
    void Take()
    {
        if (holding)
            return;
        if (pendingNew)
        {
            last = pending;
            lastSerial = pendingSerial;
            lastQueued = false;
            pending = 0;
            pendingNew = false;
            holding = holdingNew = true;
        }
        else if (last)
        {
            holding = true;
            holdingNew = false;
        }
    }

    // RunFrame's vkQueueSubmit: the read of the image taken.
    void Queue()
    {
        if (!holding)
            return;
        const uint64_t read = content[last];
        if (holdingNew)
        {
            if (read == lastSerial)
            {
                shown++;
                if (read <= lastShown)
                    outOfOrder++;
                lastShown = read;
            }
            else
                wrong++;
        }
        lastQueued = true;
        holding = holdingNew = false;
    }

    // Every frame made is shown as itself, counted as dropped, or still on
    // its way (waiting, or taken and not overwritten).
    uint64_t Unaccounted() const
    {
        const uint64_t onTheWay = (pendingNew ? 1 : 0) + (holdingNew && lastSerial != lastOverwritten ? 1 : 0);
        return serial - (shown + dropped + onTheWay);
    }
};

static Chooser RealChooser()
{
    return [](const Handle* images, int count, Handle presenter, Handle waiting) {
        return gpu::ChooseFrontImage(images, count, presenter, waiting);
    };
}

// Planted: the presenter's image not avoided (only the waiting frame's).
static Chooser IgnoresPresenter()
{
    return [](const Handle* images, int count, Handle, Handle waiting) {
        for (int i = 0; i < count; i++)
            if (images[i] != waiting)
                return gpu::FrontChoice{ i, Kind::Free };
        return gpu::FrontChoice{ 0, Kind::TakenBack };
    };
}

// Scripted: v0.1.4's race, and the ring full.
static void TestScripted()
{
    {
        // Frame 1 taken; frame 2's swap comes while the presenter waits for
        // the display, before its read is queued.
        Model m(RealChooser(), gpu::kFrontImages);
        m.Swap();
        m.Take();
        m.Swap();
        m.Queue();
        CHECK(m.wrong == 0 && m.shown == 1 && m.lastShown == 1, "a swap before the read: frame %llu shown (wrong %llu)",
            (unsigned long long)m.lastShown, (unsigned long long)m.wrong);
        m.Take();
        m.Queue();
        CHECK(m.wrong == 0 && m.shown == 2 && m.lastShown == 2 && m.dropped == 0, "then frame 2: shown %llu, last %llu, dropped %llu",
            (unsigned long long)m.shown, (unsigned long long)m.lastShown, (unsigned long long)m.dropped);
    }
    {
        // The same with one image (v0.1.4): frame 2 in frame 1's place, then
        // again; now counted as frame 1 dropped.
        Model m(RealChooser(), 1);
        m.Swap();
        m.Take();
        m.Swap();
        m.Queue();
        m.Take();
        m.Queue();
        CHECK(m.wrong == 1 && m.overwritten == 1 && m.dropped == 1 && m.shown == 1 && m.lastShown == 2,
            "one image: wrong %llu, overwritten %llu, dropped %llu, shown %llu (last %llu)", (unsigned long long)m.wrong,
            (unsigned long long)m.overwritten, (unsigned long long)m.dropped, (unsigned long long)m.shown, (unsigned long long)m.lastShown);
    }
    {
        // Two swaps while the presenter holds frame 1: frame 2 is taken back
        // for frame 3 (dropped, as replacing it would have), frame 1 is
        // still shown as itself.
        Model m(RealChooser(), gpu::kFrontImages);
        m.Swap();
        m.Take();
        m.Swap();
        m.Swap();
        m.Queue();
        m.Take();
        m.Queue();
        CHECK(m.wrong == 0 && m.dropped == 1 && m.shown == 2 && m.lastShown == 3 && m.Unaccounted() == 0,
            "ring full: wrong %llu, dropped %llu, shown %llu (last %llu)", (unsigned long long)m.wrong, (unsigned long long)m.dropped,
            (unsigned long long)m.shown, (unsigned long long)m.lastShown);
    }
    {
        // A redraw (the game presents nothing new) holds the last frame's
        // image: a swap meanwhile writes the other.
        Model m(RealChooser(), gpu::kFrontImages);
        m.Swap();
        m.Take();
        m.Queue();
        m.Take();  // a redraw of frame 1
        m.Swap();
        CHECK(m.pending != m.last, "a swap during a redraw writes another image (pending %llu, the presenter's %llu)",
            (unsigned long long)m.pending, (unsigned long long)m.last);
        m.Queue();
        m.Take();
        m.Queue();
        CHECK(m.wrong == 0 && m.shown == 2 && m.lastShown == 2, "then frame 2: shown %llu (last %llu)", (unsigned long long)m.shown,
            (unsigned long long)m.lastShown);
    }
}

// Random interleavings: the renderer swapping at its own pace, the presenter
// taking a frame and queueing its read some events later.
struct RunResult
{
    uint64_t frames, wrong, dropped, overwritten, unaccounted, outOfOrder;
};

static RunResult Run(Chooser choose, int count, uint32_t seed, int steps)
{
    Model m(std::move(choose), count);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> event(0, 99);
    RunResult r{};
    for (int i = 0; i < steps; i++)
    {
        const int e = event(rng);
        if (e < 40)
            m.Swap();
        else if (e < 70)
            m.Take();
        else
            m.Queue();
        r.unaccounted = std::max(r.unaccounted, m.Unaccounted());
    }
    r.frames = m.serial;
    r.wrong = m.wrong;
    r.dropped = m.dropped;
    r.overwritten = m.overwritten;
    r.outOfOrder = m.outOfOrder;
    return r;
}

static void TestRandom()
{
    for (int count = 2; count <= gpu::kMaxFrontImages; count++)
        for (uint32_t seed = 1; seed <= 20; seed++)
        {
            const RunResult r = Run(RealChooser(), count, seed, 20000);
            CHECK(r.wrong == 0 && r.overwritten == 0 && r.unaccounted == 0 && r.outOfOrder == 0 && r.frames > 1000,
                "%d images, seed %u: %llu frames, %llu shown as another, %llu written over, %llu unaccounted, %llu out of order", count, seed,
                (unsigned long long)r.frames, (unsigned long long)r.wrong, (unsigned long long)r.overwritten, (unsigned long long)r.unaccounted,
                (unsigned long long)r.outOfOrder);
        }
    // One image: the race happens, and each time it is counted.
    const RunResult one = Run(RealChooser(), 1, 1, 20000);
    printf("one image: %llu frames, %llu shown as another, all counted as dropped (%llu written over)\n", (unsigned long long)one.frames,
        (unsigned long long)one.wrong, (unsigned long long)one.overwritten);
    CHECK(one.wrong > 0, "one image: the model never loses the race (it should, as v0.1.4 did)");
    CHECK(one.wrong == one.overwritten && one.unaccounted == 0, "one image: %llu shown as another, %llu counted, %llu unaccounted",
        (unsigned long long)one.wrong, (unsigned long long)one.overwritten, (unsigned long long)one.unaccounted);
    // Planted: the presenter's image not avoided, with two images.
    g_quiet = true;
    const int before = g_failures;
    const RunResult planted = Run(IgnoresPresenter(), gpu::kFrontImages, 1, 20000);
    CHECK(planted.wrong == 0, "planted");
    const bool caught = g_failures > before;
    g_failures = before;
    g_quiet = false;
    printf("planted: the presenter's image not avoided: %llu frames shown as another\n", (unsigned long long)planted.wrong);
    CHECK(caught, "planted: the presenter's image not avoided: every frame still shown as itself");
}

static std::string ReadStripped(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "can't read %s", path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s;
    for (char c : ss.str())
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            s += c;
    return s;
}

// The text from `start` to the next `end` after it (empty if either is missing).
static std::string Between(const std::string& s, const std::string& start, const std::string& end)
{
    const size_t a = s.find(start);
    if (a == std::string::npos)
        return {};
    const size_t b = s.find(end, a + start.size());
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

static void TestSources(const char* rendererPath, const char* presenterPath)
{
    const std::string renderer = ReadStripped(rendererPath);
    const std::string present = Between(renderer, "boolPresentFrontBuffer(", "voidOnSwap(");
    CHECK(!present.empty(), "PresentFrontBuffer not found in %s", rendererPath);
    // (ClaimFrontSlot: video::ClaimFrontImage over the slots' current images,
    // and a target's image handed over to the slot recycled, NFSMW_RESOLVE_PRESENT=2.)
    const size_t claim = present.find("Image&front=ring.images[size_t(ClaimFrontSlot(ring))];");
    const std::string claimSlot = Between(renderer, "intClaimFrontSlot(FrontRing&ring){", "returnslot;}");
    CHECK(claimSlot.find("video::ClaimFrontImage(handles.data(),s_frontImages)") != std::string::npos,
        "ClaimFrontSlot picks the slot with video::ClaimFrontImage");
    const size_t view = present.find("VkDescriptorImageInfoii{VK_NULL_HANDLE,front.view,");
    const size_t dispatch = present.find("vkCmdDispatch(");
    const size_t submit = present.find("SubmitRecorded(7);");
    const size_t handOver = present.find("video::SubmitFrontImage(front.image,");
    CHECK(claim != std::string::npos, "PresentFrontBuffer writes the image video::ClaimFrontImage chooses");
    CHECK(claim < view && view < dispatch, "chosen before its write is recorded");
    CHECK(dispatch < submit && submit < handOver, "and handed over after the write is submitted");

    const std::string presenter = ReadStripped(presenterPath);
    const std::string claimBody = Between(presenter, "intClaimFrontImage(constVkImage*images,intcount){", "returnchoice.index;");
    CHECK(!claimBody.empty(), "ClaimFrontImage not found in %s", presenterPath);
    const size_t lock = claimBody.find("std::lock_guardlock(s_pendingMutex);");
    const size_t choose = claimBody.find("gpu::ChooseFrontImage(images,count,s_lastImage,s_pendingImage)");
    CHECK(lock != std::string::npos && choose != std::string::npos && lock < choose,
        "ClaimFrontImage chooses with the presenter's image and the waiting one under s_pendingMutex");
    CHECK(claimBody.find("s_pendingImage=VK_NULL_HANDLE;") != std::string::npos, "and takes the waiting frame back when it must");
    const std::string take = Between(presenter, "elseif(s_pendingNew){pixels.swap(s_pending);",
        "constboolupload=!pixels.empty();");
    CHECK(!take.empty(), "RunFrame's take not found");
    CHECK(take.find("gpuImage=s_pendingImage;") != std::string::npos && take.find("s_lastImage=gpuImage;") != std::string::npos,
        "the presenter's image is marked in the same hold of s_pendingMutex as the take");
    const std::string runFrame = Between(presenter, "boolRunFrame(){", "VkPresentInfoKHRpi{");
    const size_t queued = runFrame.find("vkQueueSubmit(s_queue,1,&si,f.inFlight);");
    CHECK(queued != std::string::npos && runFrame.find("s_lastQueued=true;", queued) != std::string::npos,
        "the read is marked queued after the presenter's vkQueueSubmit");
}

int main(int argc, char** argv)
{
    TestChoice();
    TestScripted();
    TestRandom();
    if (argc > 2)
        TestSources(argv[1], argv[2]);
    else
        printf("(no renderer.cpp and presenter.cpp given: their checks skipped)\n");
    if (g_failures)
    {
        printf("gpu_front_ring_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("gpu_front_ring_test: all passed\n");
    return 0;
}
