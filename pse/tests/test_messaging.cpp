// pse.messaging: listeners, Dispatch, interface registry, events (priorities, reentrancy).
#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSEMessagingInterface* M() { return pse::MessagingInterface(); }

struct Got {
    std::vector<std::string> senders;
    std::vector<uint32_t> types;
    std::vector<std::string> payloads;
};
void PSE_CALL OnMsg(const PSEMessage* msg, void* ctx) {
    auto* g = static_cast<Got*>(ctx);
    g->senders.emplace_back(msg->sender);
    g->types.push_back(msg->type);
    g->payloads.emplace_back(msg->data ? std::string(static_cast<const char*>(msg->data), msg->dataLen) : "");
}

std::vector<std::string> g_order;
struct Tag {
    const char* name;
    int ret = 0;
};
int PSE_CALL Record(PSEEventId, void*, void* ctx) {
    auto* t = static_cast<Tag*>(ctx);
    g_order.emplace_back(t->name);
    return t->ret;
}
}  // namespace

PSE_TEST("messaging: listener by sender and from anyone; broadcast and direct") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a"), b = AddPlugin("b"), c = AddPlugin("c");
    Got bFromA, cAny, bFromLoader;
    PSE_CHECK_EQ(M()->RegisterListener(b, "a", &OnMsg, &bFromA), PSE_OK);
    PSE_CHECK_EQ(M()->RegisterListener(c, nullptr, &OnMsg, &cAny), PSE_OK);
    PSE_CHECK_EQ(M()->RegisterListener(b, "pse", &OnMsg, &bFromLoader), PSE_OK);

    PSE_CHECK_EQ(M()->Dispatch(a, 7, "hi", 2, nullptr), PSE_OK);   // broadcast
    PSE_CHECK_EQ(bFromA.types.size(), (size_t)1);
    PSE_CHECK_EQ(cAny.types.size(), (size_t)1);
    PSE_CHECK_STR(bFromA.senders[0].c_str(), "a");
    PSE_CHECK_STR(bFromA.payloads[0].c_str(), "hi");
    PSE_CHECK_EQ(bFromA.types[0], 7u);

    PSE_CHECK_EQ(M()->Dispatch(a, 8, nullptr, 0, "c"), PSE_OK);    // direct: only c
    PSE_CHECK_EQ(bFromA.types.size(), (size_t)1);
    PSE_CHECK_EQ(cAny.types.size(), (size_t)2);

    PSE_CHECK_EQ(M()->Dispatch(b, 9, nullptr, 0, nullptr), PSE_OK); // from b: only c (any)
    PSE_CHECK_EQ(cAny.types.size(), (size_t)3);
    PSE_CHECK_STR(cAny.senders[2].c_str(), "b");

    pse::Broadcast(PSE_MSG_POST_LOAD, nullptr, 0);
    PSE_CHECK_EQ(bFromLoader.types.size(), (size_t)1);
    PSE_CHECK_EQ(bFromLoader.types[0], (uint32_t)PSE_MSG_POST_LOAD);
    PSE_CHECK_EQ(cAny.types.size(), (size_t)4);
    PSE_CHECK_EQ(bFromA.types.size(), (size_t)1);

    PSE_CHECK_EQ(M()->Dispatch(a, 1, nullptr, 0, "nobody"), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(M()->Dispatch(0, 1, nullptr, 0, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(M()->RegisterListener(0, nullptr, &OnMsg, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(M()->RegisterListener(a, nullptr, nullptr, nullptr), PSE_E_INVALID_ARG);
}

namespace {
PSEPluginHandle g_reentrant = 0;
Got g_late;
void PSE_CALL AddsListener(const PSEMessage*, void*) {
    M()->RegisterListener(g_reentrant, nullptr, &OnMsg, &g_late);   // not part of this dispatch
}
}  // namespace

PSE_TEST("messaging: a listener added during Dispatch gets the next message only") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a");
    g_reentrant = AddPlugin("b");
    g_late = {};
    M()->RegisterListener(g_reentrant, nullptr, &AddsListener, nullptr);
    M()->Dispatch(a, 1, nullptr, 0, nullptr);
    PSE_CHECK_EQ(g_late.types.size(), (size_t)0);
    M()->Dispatch(a, 2, nullptr, 0, nullptr);
    PSE_CHECK(g_late.types.size() >= 1);
}

PSE_TEST("messaging: interface registry - versions, duplicates, reserved ids") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a"), b = AddPlugin("b");
    static const int t1 = 1, t2 = 2, t2b = 22;
    PSE_CHECK_EQ(M()->RegisterInterface(a, "a.api", 2, &t2), PSE_OK);
    PSE_CHECK_EQ(M()->RegisterInterface(a, "a.api", 1, &t1), PSE_OK);
    PSE_CHECK_EQ(M()->RegisterInterface(b, "a.api", 2, &t2b), PSE_E_EXISTS);   // same (id, version)
    PSE_CHECK_EQ(M()->RegisterInterface(a, PSE_LOG_INTERFACE, 9, &t1), PSE_E_EXISTS);
    PSE_CHECK_EQ(M()->RegisterInterface(a, "", 1, &t1), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(M()->RegisterInterface(a, "x", 1, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(M()->RegisterInterface(0, "x", 1, &t1), PSE_E_INVALID_ARG);
    PSE_CHECK(M()->QueryInterface(b, "a.api", 0) == &t2);   // highest
    PSE_CHECK(M()->QueryInterface(b, "a.api", 1) == &t2);
    PSE_CHECK(M()->QueryInterface(b, "a.api", 2) == &t2);
    PSE_CHECK(M()->QueryInterface(b, "a.api", 3) == nullptr);
    PSE_CHECK(M()->QueryInterface(b, "missing", 0) == nullptr);
    PSE_CHECK(M()->QueryInterface(b, PSE_TASKS_INTERFACE, 1) == pse::TasksInterface());
}

PSE_TEST("messaging: event ids - built-ins, same name same id, dynamic range") {
    Fresh();
    PSE_CHECK_EQ(M()->EventId("pse.frame"), (PSEEventId)PSE_EVENT_FRAME);
    PSE_CHECK_EQ(M()->EventId("pse.pre_render"), (PSEEventId)PSE_EVENT_PRE_RENDER);
    PSE_CHECK_EQ(M()->EventId("pse.post_render"), (PSEEventId)PSE_EVENT_POST_RENDER);
    PSE_CHECK_EQ(M()->EventId("pse.end_scene"), (PSEEventId)PSE_EVENT_END_SCENE);
    PSE_CHECK_EQ(M()->EventId("pse.present"), (PSEEventId)PSE_EVENT_PRESENT);
    PSE_CHECK_EQ(M()->EventId("pse.level_load_begin"), (PSEEventId)PSE_EVENT_LEVEL_LOAD_BEGIN);
    PSE_CHECK_EQ(M()->EventId("pse.level_load_end"), (PSEEventId)PSE_EVENT_LEVEL_LOAD_END);
    PSE_CHECK_EQ(M()->EventId("pse.level_unload"), (PSEEventId)PSE_EVENT_LEVEL_UNLOAD);
    PSEEventId x = M()->EventId("ds1.script_message"), y = M()->EventId("ds1.take_damage");
    PSE_CHECK(x >= PSE_EVENT_FIRST_DYNAMIC && y >= PSE_EVENT_FIRST_DYNAMIC && x != y);
    PSE_CHECK_EQ(M()->EventId("ds1.script_message"), x);
    PSE_CHECK_EQ(M()->EventId(nullptr), 0u);
    PSE_CHECK_EQ(M()->EventId(""), 0u);
}

PSE_TEST("messaging: priorities - higher first, ties in subscription order, stop") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a");
    g_order.clear();
    Tag low{"low"}, mid1{"mid1"}, mid2{"mid2"}, high{"high"}, mid3{"mid3"};
    M()->Subscribe(a, PSE_EVENT_FRAME, &Record, &low, -5);
    M()->Subscribe(a, PSE_EVENT_FRAME, &Record, &mid1, 0);
    M()->Subscribe(a, PSE_EVENT_FRAME, &Record, &high, 100);
    M()->Subscribe(a, PSE_EVENT_FRAME, &Record, &mid2, 0);
    M()->Subscribe(a, PSE_EVENT_FRAME, &Record, &mid3, 0);
    PSE_CHECK_EQ(M()->Raise(a, PSE_EVENT_FRAME, nullptr), 0);
    std::vector<std::string> want = {"high", "mid1", "mid2", "mid3", "low"};
    PSE_CHECK(g_order == want);

    g_order.clear();
    mid2.ret = 1;   // stops the event
    PSE_CHECK_EQ(M()->Raise(a, PSE_EVENT_FRAME, nullptr), 1);
    want = {"high", "mid1", "mid2"};
    PSE_CHECK(g_order == want);
    PSE_CHECK_EQ(pse::RaiseEvent(PSE_EVENT_FRAME, nullptr), 1);
}

PSE_TEST("messaging: subscribe validation, unsubscribe by owner only") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a"), b = AddPlugin("b");
    Tag t{"t"};
    PSE_CHECK_EQ(M()->Subscribe(0, PSE_EVENT_FRAME, &Record, &t, 0), 0u);
    PSE_CHECK_EQ(M()->Subscribe(a, PSE_EVENT_FRAME, nullptr, &t, 0), 0u);
    PSE_CHECK_EQ(M()->Subscribe(a, 999, &Record, &t, 0), 0u);              // never handed out
    PSE_CHECK_EQ(M()->Subscribe(a, PSE_EVENT_FIRST_DYNAMIC + 5, &Record, &t, 0), 0u);
    PSEEventId dyn = M()->EventId("my.event");
    PSESubscriptionId s = M()->Subscribe(a, dyn, &Record, &t, 0);
    PSE_CHECK(s != 0);
    PSE_CHECK_EQ(M()->Unsubscribe(b, s), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(M()->Unsubscribe(a, 12345), PSE_E_NOT_FOUND);
    g_order.clear();
    M()->Raise(b, dyn, nullptr);
    PSE_CHECK_EQ(g_order.size(), (size_t)1);
    PSE_CHECK_EQ(M()->Unsubscribe(a, s), PSE_OK);
    PSE_CHECK_EQ(M()->Unsubscribe(a, s), PSE_E_NOT_FOUND);
    g_order.clear();
    M()->Raise(b, dyn, nullptr);
    PSE_CHECK_EQ(g_order.size(), (size_t)0);
    PSE_CHECK_EQ(M()->Raise(a, 4242, nullptr), 0);   // unknown event: nothing, no crash
}

namespace {
// Reentrancy fixture: callbacks that change the subscriber list or raise again while the event runs.
PSEPluginHandle g_p = 0;
PSESubscriptionId g_victim = 0, g_self = 0;
PSEEventId g_inner = 0;
int g_depth = 0;
Tag g_victimTag{"victim"}, g_lateTag{"late"}, g_innerTag{"inner"};
int PSE_CALL KillsVictim(PSEEventId, void*, void*) {
    g_order.emplace_back("killer");
    M()->Unsubscribe(g_p, g_victim);
    return 0;
}
int PSE_CALL AddsLate(PSEEventId e, void*, void*) {
    g_order.emplace_back("adder");
    M()->Subscribe(g_p, e, &Record, &g_lateTag, 1000);   // even at a higher priority: not in this Raise
    return 0;
}
int PSE_CALL RemovesSelf(PSEEventId, void*, void*) {
    g_order.emplace_back("self");
    M()->Unsubscribe(g_p, g_self);
    return 0;
}
int PSE_CALL RaisesAgain(PSEEventId e, void*, void*) {
    g_order.emplace_back("raiser");
    if (g_depth++ == 0) {
        M()->Raise(g_p, g_inner, nullptr);
        M()->Raise(g_p, e, nullptr);   // same event, nested
    }
    return 0;
}
}  // namespace

PSE_TEST("messaging: unsubscribing a later callback during Raise stops it running") {
    Fresh();
    g_p = AddPlugin("a");
    g_order.clear();
    M()->Subscribe(g_p, PSE_EVENT_FRAME, &KillsVictim, nullptr, 10);
    g_victim = M()->Subscribe(g_p, PSE_EVENT_FRAME, &Record, &g_victimTag, 0);
    M()->Raise(g_p, PSE_EVENT_FRAME, nullptr);
    std::vector<std::string> want = {"killer"};
    PSE_CHECK(g_order == want);
}

PSE_TEST("messaging: subscribing during Raise takes effect from the next Raise") {
    Fresh();
    g_p = AddPlugin("a");
    g_order.clear();
    PSESubscriptionId adder = M()->Subscribe(g_p, PSE_EVENT_FRAME, &AddsLate, nullptr, 0);
    M()->Raise(g_p, PSE_EVENT_FRAME, nullptr);
    PSE_CHECK(g_order == std::vector<std::string>{"adder"});
    M()->Unsubscribe(g_p, adder);
    g_order.clear();
    M()->Raise(g_p, PSE_EVENT_FRAME, nullptr);
    PSE_CHECK(g_order == std::vector<std::string>{"late"});
}

PSE_TEST("messaging: a callback can unsubscribe itself; nested Raise works") {
    Fresh();
    g_p = AddPlugin("a");
    g_order.clear();
    g_self = M()->Subscribe(g_p, PSE_EVENT_FRAME, &RemovesSelf, nullptr, 5);
    Tag after{"after"};
    M()->Subscribe(g_p, PSE_EVENT_FRAME, &Record, &after, 0);
    M()->Raise(g_p, PSE_EVENT_FRAME, nullptr);
    M()->Raise(g_p, PSE_EVENT_FRAME, nullptr);
    PSE_CHECK((g_order == std::vector<std::string>{"self", "after", "after"}));

    Fresh();
    g_p = AddPlugin("a");
    g_order.clear();
    g_depth = 0;
    g_inner = M()->EventId("test.inner");
    M()->Subscribe(g_p, g_inner, &Record, &g_innerTag, 0);
    M()->Subscribe(g_p, PSE_EVENT_PRESENT, &RaisesAgain, nullptr, 0);
    M()->Raise(g_p, PSE_EVENT_PRESENT, nullptr);
    PSE_CHECK((g_order == std::vector<std::string>{"raiser", "inner", "raiser"}));
}

PSE_TEST("messaging: data pointer reaches the callback") {
    Fresh();
    PSEPluginHandle a = AddPlugin("a");
    struct L {
        static int PSE_CALL Cb(PSEEventId e, void* data, void* ctx) {
            *static_cast<int*>(ctx) = *static_cast<int*>(data) + static_cast<int>(e);
            return 0;
        }
    };
    int out = 0, in = 40;
    M()->Subscribe(a, PSE_EVENT_LEVEL_LOAD_END, &L::Cb, &out, 0);
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_END, &in);
    PSE_CHECK_EQ(out, 40 + PSE_EVENT_LEVEL_LOAD_END);
}
