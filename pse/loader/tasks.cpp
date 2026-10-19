// pse.tasks: work queued from any thread, run by pse::Frame() on the game thread.
#include <windows.h>

#include <deque>
#include <mutex>

#include "internal.h"

namespace pse {
namespace {

struct Task {
    PSEPluginHandle plugin;
    PSETaskFn fn;
    void* ctx;
    uint32_t due;   // runs in the Frame() whose number is >= due
    bool external;  // plugin is an external one: run under the crash guard (guard.cpp)
};

std::mutex g_mutex;
std::deque<Task> g_queue;        // queue order
uint32_t g_frame = 0;            // Frame() calls since Init
DWORD g_gameThread = 0;
bool g_warnedThread = false;

PSEResult PSE_CALL RunAfterFrames(PSEPluginHandle plugin, uint32_t frames, PSETaskFn fn, void* ctx) {
    if (!detail::ValidPlugin(plugin) || !fn) return PSE_E_INVALID_ARG;
    const bool external = PluginExternal(plugin);
    std::lock_guard lock(g_mutex);
    // "Next frame" = the next Frame() call, even when queued from inside a task of the current one.
    uint32_t due = g_frame + 1 + frames;
    if (due < g_frame) due = UINT32_MAX;   // overflow: as late as possible
    g_queue.push_back({plugin, fn, ctx, due, external});
    return PSE_OK;
}

PSEResult PSE_CALL RunOnGameThread(PSEPluginHandle plugin, PSETaskFn fn, void* ctx) {
    return RunAfterFrames(plugin, 0, fn, ctx);
}

int PSE_CALL IsGameThread() { return g_gameThread != 0 && GetCurrentThreadId() == g_gameThread ? 1 : 0; }

uint32_t PSE_CALL FrameNumber() {
    std::lock_guard lock(g_mutex);
    return g_frame;
}

const PSETasksInterface kTable = {sizeof(PSETasksInterface), 1, &RunOnGameThread, &RunAfterFrames, &IsGameThread,
                                  &FrameNumber};

}  // namespace

namespace detail {

void InitTasks() {
    g_gameThread = GetCurrentThreadId();
    g_warnedThread = false;
}

void ForgetTasks(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_queue, [&](const Task& t) { return t.plugin == plugin; });
}

void ResetTasks() {
    std::lock_guard lock(g_mutex);
    g_queue.clear();
    g_frame = 0;
    g_gameThread = 0;
}

}  // namespace detail

void Frame() {
    if (!IsGameThread()) {
        if (!g_warnedThread) LogLine(detail::LoaderHandle(), PSE_LOG_ERROR, "Frame() called off the game thread - ignored");
        g_warnedThread = true;
        return;
    }
    std::vector<Task> run;
    {
        // Frame number and the set of due tasks are taken together, so a task queued from another thread now is
        // either in this batch or due next frame - never lost.
        std::lock_guard lock(g_mutex);
        ++g_frame;
        for (auto it = g_queue.begin(); it != g_queue.end();) {
            if (it->due <= g_frame) {
                run.push_back(*it);
                it = g_queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Task& t : run) {
        if (!t.external) {
            t.fn(t.ctx);
            continue;
        }
        if (PluginDisabled(t.plugin)) continue;   // one of its tasks in this batch crashed: skip the rest
        detail::Fault fault;
        if (!detail::GuardedTask(t.fn, t.ctx, &fault)) detail::PluginCrashed(t.plugin, "a pse.tasks task", fault);
    }
    RaiseEvent(PSE_EVENT_FRAME, nullptr);
}

const PSETasksInterface* TasksInterface() { return &kTable; }

}  // namespace pse
