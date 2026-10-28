// pse.tasks + pse::Frame().
#include <atomic>
#include <thread>

#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSETasksInterface* T() { return pse::TasksInterface(); }
std::vector<int> g_ran;
std::mutex g_ranMutex;
PSEPluginHandle g_p = 0;

void PSE_CALL Push(void* ctx) {
    std::lock_guard lock(g_ranMutex);
    g_ran.push_back(static_cast<int>(reinterpret_cast<intptr_t>(ctx)));
}
void* Ctx(int v) { return reinterpret_cast<void*>(static_cast<intptr_t>(v)); }

void PSE_CALL QueuesMore(void* ctx) {
    Push(ctx);
    T()->RunOnGameThread(g_p, &Push, Ctx(100));   // runs next frame, not now
}
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    Push(Ctx(-1));
    return 0;
}
}  // namespace

PSE_TEST("tasks: run on the next Frame in queue order, then the frame event") {
    Fresh();
    g_p = AddPlugin("tasker");
    g_ran.clear();
    pse::MessagingInterface()->Subscribe(g_p, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    PSE_CHECK(T()->IsGameThread());
    PSE_CHECK_EQ(T()->FrameNumber(), 0u);
    for (int i = 1; i <= 5; ++i) PSE_CHECK_EQ(T()->RunOnGameThread(g_p, &Push, Ctx(i)), PSE_OK);
    PSE_CHECK(g_ran.empty());
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{1, 2, 3, 4, 5, -1}));
    PSE_CHECK_EQ(T()->FrameNumber(), 1u);
    g_ran.clear();
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{-1}));   // tasks run once
}

PSE_TEST("tasks: a task queued while tasks run waits for the next frame") {
    Fresh();
    g_p = AddPlugin("tasker");
    g_ran.clear();
    T()->RunOnGameThread(g_p, &QueuesMore, Ctx(1));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{1}));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{1, 100}));
}

PSE_TEST("tasks: RunAfterFrames counts frames") {
    Fresh();
    g_p = AddPlugin("tasker");
    g_ran.clear();
    T()->RunAfterFrames(g_p, 2, &Push, Ctx(2));
    T()->RunAfterFrames(g_p, 0, &Push, Ctx(0));
    T()->RunAfterFrames(g_p, 1, &Push, Ctx(1));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{0}));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{0, 1}));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{0, 1, 2}));
    pse::Frame();
    PSE_CHECK_EQ(g_ran.size(), (size_t)3);
    PSE_CHECK_EQ(T()->RunAfterFrames(0, 0, &Push, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(T()->RunOnGameThread(g_p, nullptr, nullptr), PSE_E_INVALID_ARG);
}

PSE_TEST("tasks: queued from other threads, run on the game thread in order") {
    Fresh();
    g_p = AddPlugin("tasker");
    g_ran.clear();
    static DWORD gameThread;
    gameThread = GetCurrentThreadId();
    static std::atomic<int> wrongThread;
    wrongThread = 0;
    struct L {
        static void PSE_CALL Check(void* ctx) {
            if (GetCurrentThreadId() != gameThread) ++wrongThread;
            Push(ctx);
        }
    };
    std::atomic<int> notGame{0};
    constexpr int kThreads = 6, kTasks = 100;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            if (!T()->IsGameThread()) ++notGame;
            for (int i = 0; i < kTasks; ++i) T()->RunOnGameThread(g_p, &L::Check, Ctx(t * 1000 + i));
        });
    // Frames run while the threads queue: every task must run exactly once.
    for (int f = 0; f < 50; ++f) pse::Frame();
    for (auto& t : ts) t.join();
    pse::Frame();
    PSE_CHECK_EQ(notGame.load(), kThreads);
    PSE_CHECK_EQ(wrongThread.load(), 0);
    PSE_CHECK_EQ(g_ran.size(), (size_t)(kThreads * kTasks));
    // Per thread the queue order is kept.
    std::vector<int> last(kThreads, -1);
    bool ordered = true;
    for (int v : g_ran) {
        int t = v / 1000, i = v % 1000;
        if (i <= last[t]) ordered = false;
        last[t] = i;
    }
    PSE_CHECK(ordered);
}

PSE_TEST("tasks: Frame off the game thread is ignored") {
    Fresh();
    g_p = AddPlugin("tasker");
    g_ran.clear();
    T()->RunOnGameThread(g_p, &Push, Ctx(7));
    std::thread([] { pse::Frame(); }).join();
    PSE_CHECK(g_ran.empty());
    PSE_CHECK_EQ(T()->FrameNumber(), 0u);
    PSE_CHECK(AnyLineContains("off the game thread"));
    pse::Frame();
    PSE_CHECK((g_ran == std::vector<int>{7}));
}
