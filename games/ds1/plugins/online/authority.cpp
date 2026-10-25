// Online co-op: every soldier has one owner PC that runs it; the other PCs show it - Pivotal's model (Conflict: Denied
// Ops, Global Storm and this game's own DirectPlay versus: clients send their own soldier, the host runs the AI, the
// world and the mission script). Replaces the input lockstep for the soldiers (2-PC tests 2026-10-12..15: with every PC
// simulating every player from shared inputs, anything a player did that touched the AI or the world - aim locking onto
// an enemy, healing a downed squad mate, a bullet hitting a barrel - came out differently per PC and the players'
// facing, health and positions drifted apart).
//
// Owner: a squad soldier (team byte +0x10 = 0) belongs to the player in soldier +0x2B00 (set by FUN_00436f70 from the
// squad slots; 0 = player 1 = the host); everything else (enemies, followers, civilians) to the host.
//
// Puppets (soldiers this PC doesn't own): the soldier update's brain (call 0x42B4C7 -> FUN_0042b7d0) and actions
// (0x42B501 -> FUN_0042ad50: combat, the trigger) are skipped, the mover (0x42B4F1 -> FUN_00429ee0) is the versus
// client's puppet mover FUN_0041faf0 - the game's own versus client runs every soldier through it at that spot - which
// walks the soldier to the target the versus state applier FUN_0041eb90 (thiscall soldier, 0, record; ret 8) sets:
// record +0xC mask (1 movement state +0x14, 2 held item +0x1C, 4 position +0x20, 8 heading +0x2C, 0x10 aim +0x30, 0x40
// stance +0x18), +0x40..+0x4C written unconditionally (+0x44 = night vision on). Mounted soldiers (+0x267C = vehicle)
// stay normally simulated (the versus builder FUN_0041db30 skips them too; a puppet driver left the trucks standing).
//
// Records (puppetsync.h: 'L' 10 packets, every kRecordMs): per owned soldier position +0x30, heading (render instance
// +0x100 [1] via FUN_004c9a40), aim +0x328, movement state +0x2674, stance +0x348, hold state +0x2688, health +0x54, and
// the held item +0x26B0 (definition hash, inventory slot, rounds, fire mode); the newest per soldier wins. The puppet
// runs puppetsync::MakePlan's steps: hold the owner's item (FUN_0042ec10), the applier, rounds, hold state, aim mode.
// Health from the owner is the truth (a record at 0 kills the puppet through the game's own path).
//
// Damage: the soldier vtable's TakeDamage (slot 15, 0x5D891C -> FUN_00424200 thiscall(soldier, float amount, object*
// attacker), ret 8) is wrapped. A hit counts on the attacker's owner PC only (a puppet's bullets do nothing - its owner's
// copy of the shot does the damage; no attacker = the victim's owner decides); it is applied there when that PC owns the
// victim, else sent to the victim's owner ("EA D victim amount attacker"), which applies it.
// Shots: the trigger (0x4A4DB8 -> FUN_004a1ad0 thiscall(weapon, shooter, 0, 0, 0), ret 0x10) of an owned soldier is
// sent ("EA F shooter") and replayed on the other PCs' puppet (sound, flash, tracer; its damage counts nowhere).
// Mission script: the host's runs the mission. [0x60F578] = the script object (6 cdecl callbacks, +8 message(id,
// param*)); it points to a copy whose message callback is ours. Queries (the sender reads an answer back) stay local;
// every other message is the host's: delivered there at once and sent ("EA M id has w0..w7"), the joiners drop their
// own and deliver the host's (the joiners' puppets have no AI - "enemy spotted" / "waypoint reached" came only on the
// host, so scripted enemies were activated there only, 2-PC test 2026-10-12).
// Exception: a message about an action of a joiner's own soldier (pick-up, unlock, switch, custom action ...) happens
// only on that joiner - it is forwarded to the host ("EA MJ"), whose mission takes it and sends it out as usual.
// Doors: the door command FUN_0041cb30 is wrapped; a real open / close on any PC is sent ("EA DR door action").
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "features/lobbynet.h"
#include "features/overlay.h"
#include "features/puppetsync.h"

namespace {
constexpr uint32_t kSoldierVtable = 0x5D88E0, kObjects = 0x63C9A0;
constexpr uint32_t kBrainCall = 0x42B4C7, kBrain = 0x42B7D0, kActionCall = 0x42B501, kAction = 0x42AD50;
constexpr uint32_t kMoverCall = 0x42B4F1, kMover = 0x429EE0, kPuppetMover = 0x41FAF0;
constexpr uint32_t kApply = 0x41EB90, kHeading = 0x4C9A40, kNightVision = 0x435E80;
constexpr uint32_t kTakeDamageSlot = 0x5D891C, kTakeDamage = 0x424200, kHealSlot = 0x5D8918, kHeal = 0x423EB0;
constexpr uint32_t kAnimTimer = 0x63C954;
constexpr uint32_t kKnockbackSlot = 0x5D88EC, kKnockback = 0x42C040;   // slot 3: explosion push, thiscall ret 0xC
constexpr uint32_t kTriggerCall = 0x4A4DB8, kTrigger = 0x4A1AD0;
// projectiles: spawner FUN_004f3fe0 thiscall(manager [0x63C9A4], desc) ret 4; called at 0x4A179D by the grenade throw
// FUN_004a1470 thiscall(item, soldier) ret 4, and at 0x4A3201 by the gun fire FUN_004a28c0 (rockets, M203, ...).
// desc: +0x04 start, +0x20 speed, +0x24 yaw, +0x28 pitch (degrees), +0x34 flags (0x40000 = M203), +0x40 item, +0x44 owner
constexpr uint32_t kSpawn = 0x4F3FE0, kThrowSpawnCall = 0x4A179D, kFireSpawnCall = 0x4A3201, kThrow = 0x4A1470;
constexpr uint32_t kSetRounds = 0x4A4BA0, kSetFireMode = 0x4A4FB0, kM203Def = 0x619838;
// vehicles (object 0x2CE0 B, vtable 0x5D8A58): slot 0 update FUN_0043f870 (fastcall), slot 3 damage FUN_0043f3b0
// thiscall(veh, float* dir, float amount, attacker) ret 0xC; place FUN_00442650 thiscall(veh, float pos[3] cm, float
// rot[3] deg) ret 8 (the save loader's, physics vehicles: chassis + wheels); turret slew FUN_00441630 (fastcall)
constexpr uint32_t kVehicleVtable = 0x5D8A58, kVehUpdateSlot = 0x5D8A58, kVehUpdate = 0x43F870,
                   kVehDamageSlot = 0x5D8A64, kVehDamage = 0x43F3B0, kVehPlace = 0x442650, kTurretSlew = 0x441630;
constexpr uint32_t kSeatPlace = 0x442300, kMount = 0x433F00, kDismount = 0x42E850;
// Karma (stdcall, body first, metres): linear / angular velocity
constexpr uint32_t kSetLinVel = 0x58E3E0, kSetAngVel = 0x58E410, kGetLinVel = 0x58DD80, kGetAngVel = 0x58DDC0;
constexpr uint32_t kScriptObject = 0x60F578;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478;
// squad switch FUN_0044f030 (thiscall block, number) after its script veto: release the old soldier FUN_0049e740, block
// mode 2 FUN_004500b0, take the new one FUN_0044f260 (thiscall block, soldier), FUN_00434ea0 (soldier), HUD FUN_0048aab0
constexpr uint32_t kRelease = 0x49E740, kSetMode = 0x4500B0, kTakeSoldier = 0x44F260, kActivate = 0x434EA0,
                   kHudRefresh = 0x48AAB0;
// doors: FUN_0041cb30 thiscall(door, action 0 close / 1 open / else toggle-lock) ret 4 - the one command every opener uses
// (soldier "open door" 0x42A2BB, proximity 0x41CD98 / 0x41CE01, script API, switches); flags +0x54 bit 1 = open
constexpr uint32_t kDoorCommand = 0x41CB30;
constexpr uint8_t kDoorEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x14};
// cutscene skip: the movie-cancel queries FUN_00486710 (API +0x58C: fades out, [0x618348] = 1) and FUN_004867B0 (+0x590)
// ask every player block for MOVIE_CANCEL (action 0x36) through FUN_004508e0 thiscall(block, action) ret 4
constexpr uint32_t kCancelCalls[] = {0x48675F, 0x4867DE}, kActionPressed = 0x4508E0, kGameState = 0x63C948;
// MISSION FAILED screen: update FUN_00414dd0 gets the chosen item from the list input FUN_004e5d80 (thiscall list) at
// 0x414DD5: 0x443 RESTART (current level into the next-level slot, returns 3 = load), 0x444 main menu (state 0x19), 0x445
// LOAD GAME (state 9). Pop-up bar: FUN_004d33a0 thiscall(view controller [0x63C98C], text, ms).
constexpr uint32_t kFailedListCall = 0x414DD5, kListInput = 0x4E5D80, kViewController = 0x63C98C, kPopup = 0x4D33A0;
constexpr int kRestartId = 0x443, kLoadGameId = 0x445;
constexpr uint32_t kFrontEndState = 0x617C18, kScreenOf = 0x480930, kFailedScreenVtable = 0x5D8660;
constexpr DWORD kRecordMs = 50;
constexpr int kMessageWords = 8;
// held items: FUN_0042ec10 thiscall(soldier, item, immediate) ret 8 - the game's "hold this item" (connector model, the
// weapon's animation set, the chooser FUN_0042d3a0); immediate = 1 skips its network-client branch (versus messages)
constexpr uint32_t kHoldItem = 0x42EC10;

bool g_installed = false, g_on = false, g_host = false, g_delivering = false;
int g_players = 0, g_local = 0;   // g_local: this PC's first player (its packets carry it)
uint32_t g_mask = 1;                // this PC's players (lobby: one or more per PC)
uint32_t g_seq = 0;
// The mission start this PC is in: the host's lockstep seed of that level load (sent with START, LockstepSeed). Every
// command and record carries it; ones from another start are dropped - after RESTART LEVEL the PC that reloaded first
// got the other's last records of the old mission ("Foley dead") and killed the fresh Foley (soak 2026-10-15).
uint32_t g_epoch = 0, g_otherEpochs = 0, g_lastOtherEpoch = 0;   // (records / commands of another start)
uint32_t g_blockSent[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};   // last soldier id announced per block
DWORD g_lastRecords = 0;

using puppetsync::Rec;
using Puppet = puppetsync::Received;
puppetsync::Receiver g_receiver;   // the newest record per soldier (by object id)
std::map<uint16_t, Puppet>& g_puppets = g_receiver.Soldiers();
uint32_t g_itemsSelected = 0, g_itemsMissing = 0;
std::deque<std::string> g_events;      // received "EA" commands, handled on the game thread
uint32_t g_sentRecords = 0, g_appliedRecords = 0, g_damageSent = 0, g_damageApplied = 0, g_damageIgnored = 0,
         g_firesSent = 0, g_firesReplayed = 0, g_messagesSent = 0, g_messagesDelivered = 0, g_messagesDropped = 0,
         g_spawnsSent = 0, g_spawnsReplayed = 0;

uint8_t** Table(uint32_t& maxId) {
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    if (!mgr) return nullptr;
    maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    return *reinterpret_cast<uint8_t** const*>(mgr + 0x30);
}
uint8_t* ById(uint32_t id) {
    uint32_t maxId = 0;
    uint8_t** t = Table(maxId);
    return t && id <= maxId ? t[id] : nullptr;
}
bool IsSoldier(const uint8_t* o) { return o && *reinterpret_cast<const uint32_t*>(o) == kSoldierVtable; }
uint32_t IdOf(const uint8_t* o) { return o ? *reinterpret_cast<const uint32_t*>(o + 8) : 0xFFFFFFFF; }
int SquadSlot(const uint8_t* s) {   // unit +0x1C7 (-1: not a squad member)
    const uint8_t* unit = *reinterpret_cast<const uint8_t* const*>(s + 0x24);
    return unit ? *reinterpret_cast<const int8_t*>(unit + 0x1C7) : -1;
}
int OwnerOf(const uint8_t* s) {
    // squad members by squad slot, not team: Mission 1's Foley (player 2) is a prisoner on the enemy team at first
    if (!IsSoldier(s) || SquadSlot(s) < 0) return 0;
    const uint32_t o = *reinterpret_cast<const uint32_t*>(s + 0x2B00);
    return o < static_cast<uint32_t>(g_players) ? static_cast<int>(o) : 0;
}
bool Mine(int player) { return player >= 0 && player < 32 && (g_mask >> player) & 1; }
bool Local(const uint8_t* o) { return !IsSoldier(o) ? g_host : Mine(OwnerOf(o)); }
bool Mounted(const uint8_t* s) { return *reinterpret_cast<const uint32_t*>(s + 0x267C) != 0; }
bool IsPuppet(const uint8_t* s) { return g_on && IsSoldier(s) && !Local(s) && !Mounted(s); }

void SendEA(const std::string& cmd) {   // "EA X ..." -> "EA #epoch X ..."
    char tag[16];
    snprintf(tag, sizeof tag, "EA #%08X ", g_epoch);
    net::SendCommand(tag + cmd.substr(3));
}

// While this PC repeats another PC's action on that PC's soldier (a pick-up, the item in its hands), the soldier must not
// count as this PC's player: the game shows the pick-up line (bottom) and the weapon name (top bar, FUN_0044d330 at the
// end of the hold FUN_0042ec10) for player-driven soldiers - the joiner saw the host's "RPG-7 ..." while holding a
// sniper rifle (video 2026-10-22). +0x23C0 (the soldier's player block) is cleared around the call.
struct NotAPlayer {
    uint8_t* s;
    uint32_t saved;
    explicit NotAPlayer(uint8_t* soldier) : s(soldier), saved(*reinterpret_cast<uint32_t*>(soldier + 0x23C0)) {
        *reinterpret_cast<uint32_t*>(s + 0x23C0) = 0;
    }
    ~NotAPlayer() { *reinterpret_cast<uint32_t*>(s + 0x23C0) = saved; }
};

// ---- pick-ups ----
// A soldier picking an item up (weapon / Medi-Kit lying in the world): FUN_00432070 thiscall(soldier, item) - AddItem,
// the pick-up line, script message 0x03, holding it. Pivotal's versus replayed pick-ups on the other PCs through the same
// function (0x41F430). Each PC ran only its own: Foley's rifle + Medi-Kit stayed on the table on the host while the joiner
// carried them, and the host's puppet Foley had empty hands (2-home session 2026-10-22). Now the soldier's owner sends
// "EA PU <soldier id> <item id>" and every other PC picks the same item up for that soldier.
constexpr uint32_t kPickUp = 0x432070;
constexpr uint8_t kPickUpEntry[] = {0x83, 0xEC, 0x0C, 0x53, 0x55};
using PickUpFn = void(__thiscall*)(uint8_t* soldier, uint8_t* item);
void* g_pickUpNext = reinterpret_cast<void*>(kPickUp);
uint32_t g_pickUpsSent = 0, g_pickUpsApplied = 0;
bool g_pickingUp = false;
void SendEA(const std::string& cmd);
void __fastcall PickUp(uint8_t* soldier, void*, uint8_t* item) {
    const bool send = g_on && !g_pickingUp && IsSoldier(soldier) && Local(soldier) && item;
    const uint32_t itemId = item ? *reinterpret_cast<const uint32_t*>(item + 8) : 0xFFFFFFFF;
    reinterpret_cast<PickUpFn>(g_pickUpNext)(soldier, item);
    if (send && itemId != 0xFFFFFFFF) {
        SendEA("EA PU " + std::to_string(IdOf(soldier)) + " " + std::to_string(itemId));
        ++g_pickUpsSent;
    }
}
void ReplayPickUp(uint32_t soldierId, uint32_t itemId) {
    uint8_t* s = ById(soldierId);
    uint8_t* item = ById(itemId);
    if (!IsSoldier(s) || Local(s) || !item) return;
    if (*reinterpret_cast<uint8_t* const*>(item + 0x6C) == s) return;   // carried already (item owner +0x6C)
    g_pickingUp = true;
    {
        NotAPlayer quiet(s);
        reinterpret_cast<PickUpFn>(g_pickUpNext)(s, item);
    }
    g_pickingUp = false;
    ++g_pickUpsApplied;
}

// ---- cutscene skip: a vote ----
// Each PC runs the mission script's cutscenes itself, so a skip must happen on every PC at once (a joiner once skipped
// into play while the host still watched, user 2026-10-16). Online it is a vote (user 2026-10-22: "vote cut-scene
// skipping with an indicator"): a player's MOVIE_CANCEL press (Esc / pad button 0 / left mouse) is that player's vote;
// joiners send theirs to the host ("EA CSV <player>"), the host keeps the votes, tells every PC who has voted ("EA CSN
// <mask>", for the indicator) and once every player of the connected PCs has voted skips there and on every PC ("EA CS").
// Votes go when the cutscene ends. One player online = a plain skip.
bool g_skipPending = false;
DWORD g_skipAt = 0;
uint32_t g_skipsSent = 0, g_skipsTaken = 0;
uint32_t g_votes = 0;              // players who voted to skip this cutscene (joiners: the host's count + their own)
DWORD g_voteSent[4] = {};          // a joiner's last vote request per player
bool g_wasCutscene = false;
DWORD g_cutsceneSince = 0;
bool InCutscene() {
    const uint8_t* state = *reinterpret_cast<const uint8_t* const*>(kGameState);
    return state && (state[0x1B8C] & 1);
}
void SkipNow(const char* why) {   // this PC skips the cutscene running now (the next cancel query answers yes)
    features::OnlineLogLine(why);
    g_skipPending = true;
    g_skipAt = GetTickCount();
}
// The players whose vote counts: every lobby player whose PC is still connected (the host's always).
uint32_t VotersMask() {
    const lobby::Table& t = lobbynet::Current();
    uint32_t pcs = 1u << 1;   // the host's and this PC's (a joiner's peer list holds only the host)
    if (lobbynet::MyPc() > 0 && lobbynet::MyPc() < 32) pcs |= 1u << lobbynet::MyPc();
    for (const auto& peer : net::Snap().peers)
        if (peer.connected && peer.pc > 0 && peer.pc < 32) pcs |= 1u << peer.pc;
    uint32_t mask = 0;
    for (int k = 0; k < lobby::kMaxPlayers && k < g_players; ++k)
        if (t.slots[k].used && t.slots[k].pc > 0 && t.slots[k].pc < 32 && ((pcs >> t.slots[k].pc) & 1)) mask |= 1u << k;
    return mask ? mask : (g_players > 0 ? (1u << g_players) - 1 : 1u);
}
int Count(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}
void HostVote(int player, const char* who) {   // host: a player's vote, then the skip once everyone has voted
    if (!InCutscene() || player < 0 || player >= 32 || g_skipPending) return;
    const uint32_t before = g_votes;
    g_votes |= 1u << player;
    const uint32_t need = VotersMask();
    char line[128];
    if (g_votes != before) {
        snprintf(line, sizeof line, "Authority: skip vote from player %d (%s) - %d of %d", player + 1, who,
                 Count(g_votes & need), Count(need));
        features::OnlineLogLine(line);
        snprintf(line, sizeof line, "EA CSN %X", g_votes);
        SendEA(line);
    }
    if ((g_votes & need) == need) {
        ++g_skipsSent;
        SendEA("EA CS");
        SkipNow("Authority: every player voted - cutscene skipped");
        g_votes = 0;
    }
}
int PlayerOfBlock(const void* block) {
    const intptr_t off = reinterpret_cast<intptr_t>(block) - static_cast<intptr_t>(kBlocks);
    return off >= 0 && off % kBlockSize == 0 && off / kBlockSize < 4 ? static_cast<int>(off / kBlockSize) : -1;
}
void LocalVote(int p);
int __fastcall CancelPressed(void* block, void*, int action) {
    const int own = reinterpret_cast<int(__thiscall*)(void*, int)>(kActionPressed)(block, action);
    if (!g_on) return own;
    const DWORD now = GetTickCount();
    const int p = PlayerOfBlock(block);
    if (own) LocalVote(p);
    if (g_skipPending && (now - g_skipAt > 5000 || !InCutscene())) g_skipPending = false;
    return g_skipPending ? 1 : 0;
}
void LocalVote(int p) {   // a press of this PC's player p
    const DWORD now = GetTickCount();
    if (p >= 0 && p < 4 && Mine(p) && InCutscene()) {
        if (g_host) {
            HostVote(p, "this PC");
        } else if (!((g_votes >> p) & 1) && now - g_voteSent[p] > 1000) {
            g_voteSent[p] = now;
            g_votes |= 1u << p;   // shown at once; the host's count follows ("EA CSN")
            SendEA("EA CSV " + std::to_string(p));
            features::OnlineLogLine(("Authority: skip vote for player " + std::to_string(p + 1) + " sent to the host").c_str());
        }
    }
}
// Every frame: the votes end with the cutscene.
void VoteFrame() {
    const bool cut = InCutscene();
    if (cut && !g_wasCutscene) g_cutsceneSince = GetTickCount();
    if (!cut && g_wasCutscene) {
        g_votes = 0;
        for (DWORD& t : g_voteSent) t = 0;
        if (g_host) SendEA("EA CSN 0");
    }
    g_wasCutscene = cut;
}

// The indicator (bottom right, over the letterbox) while a cutscene runs online: "SKIP CUTSCENE x/y", the key and one
// box per voter in the lobby colours, filled once that player has voted.
void VoteDraw() {
    if (!g_on || !InCutscene() || g_skipPending || GetTickCount() - g_cutsceneSince < 600) return;
    const uint8_t* r = *reinterpret_cast<uint8_t* const*>(0x63C924);
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const float w = static_cast<float>(*reinterpret_cast<const uint32_t*>(r + 0x40688));
    const float h = static_cast<float>(*reinterpret_cast<const uint32_t*>(r + 0x4068C));
    const uint32_t need = VotersMask();
    const bool voted = (g_votes & g_mask) == g_mask;
    const std::string shown = "SKIP CUTSCENE " + std::to_string(Count(g_votes & need)) + "/" + std::to_string(Count(need)) +
                              (voted ? " - WAITING FOR THE OTHERS" : " - PRESS ESC / A / CROSS TO VOTE");
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(font, "M")));
    const float tw = static_cast<float>(overlay::TextWidth(font, shown.c_str()));
    const float box = line * 1.1f, gap = line * 0.5f;
    const float total = tw + gap + Count(need) * (box + gap);
    const float x = w - total - w * 0.03f, y = h - h * 0.045f;
    overlay::QueueScreenRect(x - gap, y - line * 1.5f, total + gap * 2, line * 2.2f, 0xA0000000);
    const lobby::Table& t = lobbynet::Current();
    float bx = x + tw + gap;
    for (int k = 0; k < 32; ++k) {
        if (!((need >> k) & 1)) continue;
        const uint32_t c = 0xFF000000 | (k < lobby::kMaxPlayers && t.slots[k].used ? lobby::kPalette[t.slots[k].colour] : 0xFFFFFF);
        const float by = y - line * 1.05f, th = std::max(2.0f, line / 6);
        if ((g_votes >> k) & 1) {
            overlay::QueueScreenRect(bx, by, box, box, c);
        } else {
            overlay::QueueScreenRect(bx, by, box, th, c);
            overlay::QueueScreenRect(bx, by + box - th, box, th, c);
            overlay::QueueScreenRect(bx, by, th, box, c);
            overlay::QueueScreenRect(bx + box - th, by, th, box, c);
        }
        bx += box + gap;
    }
    overlay::Flush();
    overlay::DrawLabel(font, shown.c_str(), static_cast<int>(x), static_cast<int>(y), voted ? 0xFFB0B0B0 : 0xFFFFFFFF);
}

// ---- restart after MISSION FAILED ----
// Every level the host loads is mirrored to the joiners (online.cpp START), so only the host may restart. A joiner's
// RESTART restarted only its own game, which then waited for a host that never came; when the host restarted too, the
// joiner got the host's START and loaded the level a second time while the host waited (user, 2026-10-16). Now a
// joiner's RESTART asks the host ("EA RR"), and the host's MISSION FAILED screen takes it as its own RESTART; a
// joiner's LOAD GAME is refused (only the host's save counts).
DWORD g_restartRequestAt = 0;
void Notice(const char* text) {
    if (void* c = *reinterpret_cast<void**>(kViewController))
        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(kPopup)(c, text, 4000);
    features::OnlineLogLine(text);
}
int __fastcall FailedListInput(void* list) {
    int id = reinterpret_cast<int(__thiscall*)(void*)>(kListInput)(list);
    if (!g_on) return id;
    if (!g_host) {
        if (static_cast<int16_t>(id) == kRestartId) {
            SendEA("EA RR");
            Notice("Online: asked the host to restart the mission");
            return 0;
        }
        if (static_cast<int16_t>(id) == kLoadGameId) {
            Notice("Online: only the host can load a game");
            return 0;
        }
        return id;
    }
    if (g_restartRequestAt && GetTickCount() - g_restartRequestAt < 60000) {   // a joiner asked: restart here
        g_restartRequestAt = 0;
        features::OnlineLogLine("Online: restarting the mission at a player's request");
        return kRestartId;
    }
    return id;
}

// ---- movement state of puppets ----
// SetMovementState FUN_0042d240 thiscall(soldier, state) ret 4 (writes +0x2674, restarts the state's timer +0x2694; 18
// callers). The puppet mover runs the game's transition rules (FUN_004265c0) with no input on a puppet, which turned
// the owner's crouched walk / strafe (states 5, 6) back into crouched standing (2) every frame - the puppet slid along
// without moving its legs (user, 2026-10-16; log: owner 2 / 5 / 6, its puppet only 2 / 3). In Pivotal's versus the
// same function returned early on a network client for an unchanged state. A puppet's state now changes only through
// the owner's records (the applier, g_applying) and the game's own deliveries (death, revive: g_delivering).
constexpr uint32_t kSetMoveState = 0x42D240, kIsNetworkGame = 0x4608E0;
constexpr uint8_t kSetMoveEntry[] = {0x56, 0x57, 0x8B, 0xF1, 0xE8};   // push esi; push edi; mov esi, ecx; call 0x4608e0
bool g_applying = false;
uint8_t* g_setMoveTramp = nullptr;
// Hold state +0x2688 (0 lowered, 1 raising, 2 ready, 6 / 7 reloading, 8 ...; SetHoldState FUN_0042dcb0 thiscall(soldier,
// state) ret 4). SetMovementState lowers the weapon for some states (0x42D2CD: FUN_0042dab0 -> SetHoldState(0)); the
// owner's action code raises it again, a puppet has none - its weapon stayed lowered for good (2-PC log 2026-10-16:
// owner held 2 for minutes, its puppet 0; the puppet played the lowered-weapon animation set, no aim pose). The
// applier's own hold path is gated by FUN_0042da90 (movement state table) - set directly instead.
constexpr uint32_t kSetHoldState = 0x42DCB0;
void KeepHoldState(uint8_t* s) {
    auto it = g_puppets.find(static_cast<uint16_t>(IdOf(s)));
    if (it == g_puppets.end() || !it->second.seq) return;
    const int want = it->second.rec.hold;
    if (want < 0 || want > 0x19 || *reinterpret_cast<const int32_t*>(s + 0x2688) == want) return;
    reinterpret_cast<void(__thiscall*)(void*, int)>(kSetHoldState)(s, want);
}
uint32_t g_moveBlocked = 0;
void __fastcall SetMoveState(uint8_t* s, void*, int state) {
    // only in play: during the level load every soldier gets its first state here - blocked, the joiner's (whose enemies
    // are all puppets) crashed in the load (onlinefix7, user 2026-10-16 + soak)
    const bool playing = *reinterpret_cast<const int8_t*>(0x60EC8C) == 0;
    if (g_on && playing && !g_applying && !g_delivering && IsPuppet(s)) {   // (an unchanged state too: it restarted the animation)
        ++g_moveBlocked;
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(g_setMoveTramp)(s, state);
    if (g_on && IsPuppet(s)) KeepHoldState(s);
}
bool HookSetMoveState() {
    if (g_setMoveTramp) return true;
    if (!patch::Matches(kSetMoveState, kSetMoveEntry, sizeof kSetMoveEntry)) return false;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    memcpy(t, kSetMoveEntry, 4);   // push esi; push edi; mov esi, ecx
    t[4] = 0xE8;                   // call 0x4608e0 (relocated)
    const int32_t call = static_cast<int32_t>(kIsNetworkGame - (reinterpret_cast<uint32_t>(t) + 9));
    memcpy(t + 5, &call, 4);
    t[9] = 0xE9;                   // jmp 0x42d249
    const int32_t back = static_cast<int32_t>((kSetMoveState + 9) - (reinterpret_cast<uint32_t>(t) + 14));
    memcpy(t + 10, &back, 4);
    g_setMoveTramp = t;
    patch::WriteJump(kSetMoveState, reinterpret_cast<const void*>(&SetMoveState));
    return true;
}

// ---- puppets ----
void __fastcall Brain(uint8_t* s) {
    if (!IsPuppet(s)) reinterpret_cast<void(__thiscall*)(void*)>(kBrain)(s);
}
void __fastcall Action(uint8_t* s) {
    if (!IsPuppet(s)) reinterpret_cast<void(__thiscall*)(void*)>(kAction)(s);
}
void __fastcall Mover(uint8_t* s) {
    reinterpret_cast<void(__thiscall*)(void*)>(IsPuppet(s) ? kPuppetMover : kMover)(s);
}
// The hold-state machine FUN_004278b0 (26 events from the controlling player's input: raise, fire, reload, throw ...
// -> FUN_00427420 -> SetHoldState), called by the puppet mover at 0x41FB6E every frame. A puppet's block gets no
// input on this PC, so it lowered the weapon (hold 0) every frame right after the owner's record had raised it (2-PC
// logs 2026-10-22: owner hold 2 for minutes, its puppet logged 0 - the rifle hung straight down in both videos). A
// puppet's hold state comes from its owner's records only.
constexpr uint32_t kHoldMachineCall = 0x41FB6E, kHoldMachine = 0x4278B0;
void __fastcall HoldMachine(uint8_t* s) {
    if (!IsPuppet(s)) reinterpret_cast<void(__thiscall*)(void*)>(kHoldMachine)(s);
}

uint32_t GameMs() {   // the timer the medikit and the get-up rule use ([0x63C954] +0x10)
    const uint8_t* t = *reinterpret_cast<uint8_t* const*>(kAnimTimer);
    return t ? *reinterpret_cast<const uint32_t*>(t + 0x10) : 0;
}

using DamageFn = void(__thiscall*)(void*, float, void*);
void Kill(uint8_t* s) {   // through the game's own path: death animation, KILLED message, mission logic
    float& health = *reinterpret_cast<float*>(s + 0x54);
    *reinterpret_cast<uint32_t*>(s + 0x25D4) = 0;   // (no invulnerability window)
    g_delivering = true;
    reinterpret_cast<DamageFn>(kTakeDamage)(s, health + 1000.0f, nullptr);
    g_delivering = false;
}

uint8_t* BlockOf(const uint8_t* s) {   // the player block controlling this soldier, if any
    for (int p = 0; p < g_players && p < 4; ++p) {
        auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
        if (*reinterpret_cast<const uint8_t* const*>(block + 0x310) == s) return block;
    }
    return nullptr;
}

// An item as puppetsync sees it: definition +0x24 -> name hash +0x80; weapons (+4 = 6) with fire mode +0x5C and the
// counters +0x54 / +0x78 / +0x80
puppetsync::Item ReadItem(const uint8_t* item) {
    puppetsync::Item it;
    const uint8_t* def = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
    if (!def) return it;
    it.hash = *reinterpret_cast<const uint32_t*>(def + 0x80);
    it.weapon = *reinterpret_cast<const uint32_t*>(item + 4) == 6;
    if (it.weapon) {
        it.fireMode = static_cast<uint16_t>(*reinterpret_cast<const uint32_t*>(item + 0x5C));
        it.rounds[0] = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(item + 0x54));
        it.rounds[1] = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(item + 0x78));
        it.rounds[2] = static_cast<int16_t>(*reinterpret_cast<const int32_t*>(item + 0x80));
    }
    return it;
}
uint8_t* InventoryItem(const uint8_t* s, int slot) {
    uint8_t* const* inv = *reinterpret_cast<uint8_t* const* const*>(s + 0x26B8);
    return inv && slot >= 0 && slot < puppetsync::kInventorySlots ? inv[slot] : nullptr;
}
puppetsync::View ReadView(const uint8_t* s) {
    puppetsync::View v;
    v.move = *reinterpret_cast<const int32_t*>(s + 0x2674);
    v.stance = *reinterpret_cast<const int32_t*>(s + 0x348);
    v.hold = *reinterpret_cast<const int32_t*>(s + 0x2688);
    v.step = *reinterpret_cast<const int32_t*>(s + 0x2A60);
    const uint8_t* block = BlockOf(s);
    v.aimMode = block ? *reinterpret_cast<const int32_t*>(block + 0x318) : -1;
    const uint8_t* held = *reinterpret_cast<uint8_t* const*>(s + 0x26B0);
    v.held = ReadItem(held);
    for (int k = 0; k < puppetsync::kInventorySlots; ++k) {
        const uint8_t* item = InventoryItem(s, k);
        v.inventory[k] = ReadItem(item);
        if (held && item == held) v.heldSlot = k;
    }
    return v;
}

void ApplyRecord(uint8_t* s, Puppet& p) {
    const Rec& in = p.rec;
    float& health = *reinterpret_cast<float*>(s + 0x54);
    // the owner's pending medikit heal (+0x2B84): the soldier panel's bar draws it as the part still healing (user,
    // 2026-10-16: "client doesn't see the healing process"); set before the downed early-out - a medikit on a downed
    // soldier is the main case. A medikit used here on top of it is forwarded by ForwardPendingHeals.
    *reinterpret_cast<float*>(s + 0x2B84) = in.pending > 0.0f ? in.pending : 0.0f;
    const bool dying = in.health <= 0.0f;
    if (dying && health <= 0.0f) return;   // dead here too: the body stays where it fell
    if (dying) {
    } else if (health > 0.0f) {
        health = in.health;
    } else {   // revived on the owner's PC: up again through the game's heal (message 0x0B, getting up)
        // The get-up rule (FUN_004265c0, run by the puppet mover too: 0x41FB67) stands a soldier up 3 s after the
        // medikit's revive time +0x2B7C once health > 0. A puppet's was stale (the medikit ran on the owner's PC), so
        // it stood up at once while the owner's copy waited the 3 s (user, 2026-10-16) - the medikit's two writes here
        // (unless this PC's own medikit just set them).
        uint32_t& revivedAt = *reinterpret_cast<uint32_t*>(s + 0x2B7C);
        if (GameMs() - revivedAt > 3000) revivedAt = GameMs();
        *reinterpret_cast<uint32_t*>(s + 0x284) |= 4;
        g_delivering = true;
        reinterpret_cast<void(__thiscall*)(void*, float)>(kHeal)(s, in.health);
        g_delivering = false;
    }
    const puppetsync::Plan plan = puppetsync::MakePlan(in, ReadView(s));
    // 1. the owner's item in the puppet's hands (the game's own hold: connector model + the weapon's animation set)
    if (plan.select) {
        uint8_t* item = plan.selectNone ? nullptr : InventoryItem(s, plan.selectSlot);
        if (plan.selectNone || item) {
            g_applying = true;
            {
                NotAPlayer quiet(s);
                reinterpret_cast<void(__thiscall*)(void*, void*, int)>(kHoldItem)(s, item, 1);
            }
            g_applying = false;
            ++g_itemsSelected;
        }
    } else if (plan.itemMissing && (g_itemsMissing++ % 200) == 0) {
        char line[128];
        snprintf(line, sizeof line, "Authority: soldier %u's owner holds item %08X, which its puppet doesn't carry",
                 IdOf(s), in.itemHash);
        features::OnlineLogLine(line);
    }
    // 2. the versus applier. Each field goes in whenever the puppet differs from its owner, not only when the owner's
    // value changes: the applier takes a hold state only while FUN_0042da90 allows a switch, so a change sent once was
    // lost for good (user, 2026-10-16). +0x2A60 (mask 0x80): the puppet mover FUN_0041faf0 branches on it (2 ->
    // FUN_0045c0f0, else the owner's turn); without it the puppet's stale value drove the feet (crouched soldiers slid).
    uint8_t r[0x50] = {};
    *reinterpret_cast<int32_t*>(r + 0x10) = in.step;
    *reinterpret_cast<uint32_t*>(r + 0x0C) = plan.applierMask;
    *reinterpret_cast<int32_t*>(r + 0x14) = in.move;
    *reinterpret_cast<int32_t*>(r + 0x18) = in.stance;
    *reinterpret_cast<int32_t*>(r + 0x1C) = in.hold;
    memcpy(r + 0x20, in.pos, 12);
    memcpy(r + 0x2C, &in.heading, 4);
    memcpy(r + 0x30, in.aim, 12);
    // +0x44: night vision as it is (the applier switches it off when 0)
    *reinterpret_cast<int32_t*>(r + 0x44) = reinterpret_cast<int(__thiscall*)(void*, int)>(kNightVision)(s, 0);
    g_applying = true;
    reinterpret_cast<void(__thiscall*)(void*, int, void*)>(kApply)(s, 0, r);
    // 3. the held weapon's fire mode (the game's setter: the M203 swaps its pools) and its counters
    if (uint8_t* weapon = *reinterpret_cast<uint8_t**>(s + 0x26B0); weapon && (plan.setFireMode || plan.setRounds)) {
        if (plan.setFireMode) reinterpret_cast<int(__thiscall*)(void*, uint32_t, int)>(kSetFireMode)(weapon, in.fireMode, 0);
        if (plan.setRounds && ReadItem(weapon).hash == in.itemHash) {
            *reinterpret_cast<int32_t*>(weapon + 0x54) = in.rounds[0];
            *reinterpret_cast<int32_t*>(weapon + 0x78) = in.rounds[1];
            *reinterpret_cast<int32_t*>(weapon + 0x80) = in.rounds[2];
        }
    }
    // 4. the hold state
    if (plan.setHold) KeepHoldState(s);
    g_applying = false;
    // 5. The aiming pose follows the controlling player's block mode (+0x318 = 1, set by FUN_004500b0). In owner mode
    // the other players' blocks get no input on this PC, so their soldiers never aimed here (user, 2026-10-16:
    // "aiming animations stopped working on both ends") - the owner's mode, through the game's setter.
    if (plan.setAimMode)
        if (uint8_t* block = BlockOf(s))
            reinterpret_cast<void(__thiscall*)(void*, int)>(kSetMode)(block, in.aimMode);
    ++g_appliedRecords;
    // dead on the owner's PC: killed here at the owner's last place (killed where the puppet happened to be, a body
    // thrown by a blast lay 6.6 m from the owner's - soak 2026-10-15; user: bodies not where the other PC had them)
    if (dying) {
        // straight there: every position copy of the soldier and its render instance (lockstep.cpp's correction list)
        float dp[3];
        for (int k = 0; k < 3; ++k) dp[k] = in.pos[k] - reinterpret_cast<const float*>(s + 0x30)[k];
        for (uint32_t off : {0x30u, 0xA0u, 0x1FCu, 0x224u, 0x240u, 0x368u, 0x384u, 0x390u})
            for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(s + off)[k] += dp[k];
        if (uint8_t* inst = *reinterpret_cast<uint8_t**>(s + 0x2C))
            for (uint32_t off : {0xF4u, 0x13Cu})
                for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(inst + off)[k] += dp[k];
        Kill(s);
    }
}

// ---- damage ----
void __fastcall TakeDamage(uint8_t* s, void*, float amount, uint8_t* attacker) {
    if (!g_on || g_delivering) return reinterpret_cast<DamageFn>(kTakeDamage)(s, amount, attacker);
    const bool counts = IsSoldier(attacker) ? Local(attacker) : Local(s);
    if (!counts) {   // a puppet's shot (its owner's copy does the damage) / the world on another PC
        ++g_damageIgnored;
        return;
    }
    if (Local(s)) {
        reinterpret_cast<DamageFn>(kTakeDamage)(s, amount, attacker);
        return;
    }
    char cmd[96];
    uint32_t bits;
    memcpy(&bits, &amount, 4);
    snprintf(cmd, sizeof cmd, "EA D %u %08X %d", IdOf(s), bits, attacker ? static_cast<int>(IdOf(attacker)) : -1);
    SendEA(cmd);
    ++g_damageSent;
}

// Heal (slot 14, 0x5D8918 -> FUN_00423eb0 thiscall(soldier, float amount), ret 4: health += amount up to the maximum,
// message 0x0B): a medikit used on another player's / the host's soldier goes to that soldier's owner (2-PC test
// 2026-10-15: healing a downed squad mate desynced until he was reselected).
using HealFn = void(__thiscall*)(void*, float);
void __fastcall Heal(uint8_t* s, void*, float amount) {
    if (!g_on || g_delivering || Local(s)) return reinterpret_cast<HealFn>(kHeal)(s, amount);
    char cmd[64];
    uint32_t bits;
    memcpy(&bits, &amount, 4);
    snprintf(cmd, sizeof cmd, "EA H %u %08X", IdOf(s), bits);
    SendEA(cmd);
}

// Medikits: the medikit (0x4A0D90..0x4A0E00, run on the healer's PC) doesn't heal - it adds the amount to the
// target's pending heal +0x2B84 (+0x2B74 = now) and, on a downed target (health <= 0), sets +0x284 |= 4 and the revive
// time +0x2B7C. The target's own update drips it into its health through Heal (FUN_00407c50 from the actions
// 0x42AD88 / 0x407B64) - skipped for a puppet, so a joiner's medikit on the host's soldier never healed anything (user,
// 2026-10-16: "client healed Bradley but the heal won't register"; the host's HUD bar showed the stuck pending heal).
// A puppet's pending heal goes to its owner ("EA MK soldier amount"), who does the medikit's writes on its copy.
uint32_t g_healsForwarded = 0;
void ForwardPendingHeals() {
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* s = table[id];
        if (!IsPuppet(s)) continue;
        // the owner's pending heal (from its records) is shown as it is; only what this PC's medikit added on top goes
        float& pending = *reinterpret_cast<float*>(s + 0x2B84);
        auto it = g_puppets.find(static_cast<uint16_t>(id));
        const float owners = it != g_puppets.end() && it->second.rec.pending > 0.0f ? it->second.rec.pending : 0.0f;
        const float added = pending - owners;
        if (!(added > 0.01f)) continue;
        char cmd[64];
        uint32_t bits;
        memcpy(&bits, &added, 4);
        snprintf(cmd, sizeof cmd, "EA MK %u %08X", id, bits);
        SendEA(cmd);
        pending = owners;
        ++g_healsForwarded;
    }
}
void ApplyMedikit(uint8_t* s, float amount) {   // the medikit's writes on the owner's copy
    if (*reinterpret_cast<float*>(s + 0x54) <= 0.0f) {
        *reinterpret_cast<uint32_t*>(s + 0x284) |= 4;
        *reinterpret_cast<uint32_t*>(s + 0x2B7C) = GameMs();
    }
    *reinterpret_cast<float*>(s + 0x2B84) += amount;
    *reinterpret_cast<uint32_t*>(s + 0x2B74) = GameMs();
}

// Animation log (online log, every build): each player-driven soldier's state on this PC whenever it changes - owner's
// copy or puppet, block mode +0x318 (0 normal, 1 aiming), movement state +0x2674, stance +0x348, held item +0x2688,
// step +0x2A60, and the two animation channels of its render instance (+0x248 + ch * 0x34: +0 animation, +0x24
// playing, +0xC time; the animation object's +0 = hash of its .prb name, the same on every PC). The two PCs' logs side by side
// show whether a puppet plays what its owner plays (user, 2026-10-16: aim pose missing, crouched soldiers sliding).
uint32_t g_animLines = 0;
void ReadChannels(const uint8_t* s, uint32_t* hash, float* t) {   // (guarded: an instance may be mid-change)
    __try {
        if (const uint8_t* inst = *reinterpret_cast<const uint8_t* const*>(s + 0x2C))
            for (int c = 0; c < 2; ++c) {
                const uint8_t* chan = inst + 0x248 + c * 0x34;
                if (*reinterpret_cast<const uint32_t*>(chan + 0x24))   // (+0x24 = playing, +0 = the animation)
                    if (const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(chan))
                        hash[c] = *reinterpret_cast<const uint32_t*>(a);
                t[c] = *reinterpret_cast<const float*>(chan + 0xC);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void LogAnims() {
    static std::map<uint32_t, std::string> last;
    static uint32_t epoch = 0;
    if (epoch != g_epoch) epoch = g_epoch, last.clear();
    if (g_animLines > 40000) return;
    for (int p = 0; p < g_players && p < 4; ++p) {
        const uint8_t* block = reinterpret_cast<const uint8_t*>(kBlocks + p * kBlockSize);
        const uint8_t* s = *reinterpret_cast<const uint8_t* const*>(block + 0x310);
        if (!IsSoldier(s)) continue;
        char key[200], ch[2][48] = {};
        uint32_t hash[2] = {};
        float t[2] = {};
        ReadChannels(s, hash, t);
        for (int c = 0; c < 2; ++c) snprintf(ch[c], sizeof ch[c], "%08X", hash[c]);
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(s + 0x26B0);
        const uint8_t* def = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
        snprintf(key, sizeof key, "%s mode %d move %d stance %d held %d item %08X step %d anim %s / %s",
                 IsPuppet(s) ? "puppet" : Local(s) ? "own" : "mounted",
                 *reinterpret_cast<const int32_t*>(block + 0x318), *reinterpret_cast<const int32_t*>(s + 0x2674),
                 *reinterpret_cast<const int32_t*>(s + 0x348), *reinterpret_cast<const int32_t*>(s + 0x2688),
                 def ? *reinterpret_cast<const uint32_t*>(def + 0x80) : 0u,
                 *reinterpret_cast<const int32_t*>(s + 0x2A60), ch[0], ch[1]);
        std::string& was = last[IdOf(s)];
        if (was == key) continue;
        was = key;
        char line[300];
        snprintf(line, sizeof line, "Anim: P%d soldier %u %s (t %.2f / %.2f, health %.1f)", p + 1, IdOf(s), key, t[0], t[1],
                 *reinterpret_cast<const float*>(s + 0x54));
        features::OnlineLogLine(line);
        ++g_animLines;
    }
}

// Knockback (slot 3, FUN_0042c040, after the damage of an explosion FUN_004ea360): a puppet is pushed on its owner's
// PC and follows that in the records - a replayed grenade's blast here threw the puppet 3.4 m off (soak 2026-10-15).
int __fastcall Knockback(uint8_t* s, void*, uint32_t a, uint32_t b, uint32_t c) {
    if (IsPuppet(s) && !g_delivering) return 0;
    return reinterpret_cast<int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(kKnockback)(s, a, b, c);
}

// ---- shots ----
using TriggerFn = int(__thiscall*)(void*, void*, uint32_t, uint32_t, uint32_t);
int __fastcall Trigger(void* weapon, void*, uint8_t* shooter, uint32_t a, uint32_t b, uint32_t c) {
    const int r = reinterpret_cast<TriggerFn>(kTrigger)(weapon, shooter, a, b, c);
    if (g_on && !g_delivering && IsSoldier(shooter) && Local(shooter)) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "EA F %u", IdOf(shooter));
        SendEA(cmd);
        ++g_firesSent;
    }
    return r;
}

// ---- grenades / rockets ----
// The owner's spawn values go with "EA P soldier defhash mode x y z speed yaw pitch" (floats as hex bits); another PC's
// puppet then throws (FUN_004a1470) / fires (the "EA F" replay) through the game's own code with those values put into
// the spawn description (the throw's spread and the puppet's own aim drew a different path). Guns' bullets aren't sent.
struct Spawn { bool pending = false; float pos[3], speed, yaw, pitch; };
Spawn g_spawn;   // the values for the next spawn of a replay (g_delivering)
const uint8_t* DefOf(const uint8_t* desc) {
    if (*reinterpret_cast<const uint32_t*>(desc + 0x34) & 0x40000) return *reinterpret_cast<uint8_t* const*>(kM203Def);
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(desc + 0x40);
    return item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
}
bool Replayed(const uint8_t* def) {   // grenades (7 frag, 0xB smoke, 0xD), rockets 5, M203 8
    if (!def) return false;
    const uint32_t c = *reinterpret_cast<const uint32_t*>(def + 0xC0);
    return c == 5 || c == 7 || c == 8 || c == 0xB || c == 0xD;
}
using SpawnFn = void*(__thiscall*)(void*, uint8_t*);
void* __fastcall SpawnProjectile(void* manager, void*, uint8_t* desc) {
    if (g_on && g_delivering && g_spawn.pending) {   // a replay: the owner's values
        g_spawn.pending = false;
        ++g_spawnsReplayed;
        memcpy(desc + 4, g_spawn.pos, 12);
        memcpy(desc + 0x20, &g_spawn.speed, 4);
        memcpy(desc + 0x24, &g_spawn.yaw, 4);
        memcpy(desc + 0x28, &g_spawn.pitch, 4);
        return reinterpret_cast<SpawnFn>(kSpawn)(manager, desc);
    }
    const uint8_t* owner = *reinterpret_cast<uint8_t* const*>(desc + 0x44);
    const uint8_t* def = DefOf(desc);
    if (g_on && !g_delivering && IsSoldier(owner) && Local(owner) && Replayed(def)) {
        const uint8_t* item = *reinterpret_cast<uint8_t* const*>(desc + 0x40);
        const uint32_t mode = item ? *reinterpret_cast<const uint32_t*>(item + 0x5C) : 0;
        uint32_t f[6];
        memcpy(f, desc + 4, 12);
        memcpy(f + 3, desc + 0x20, 12);
        char cmd[160];
        snprintf(cmd, sizeof cmd, "EA P %u %08X %u %08X %08X %08X %08X %08X %08X", IdOf(owner),
                 *reinterpret_cast<const uint32_t*>(def + 0x80), mode, f[0], f[1], f[2], f[3], f[4], f[5]);
        SendEA(cmd);
        ++g_spawnsSent;
    }
    return reinterpret_cast<SpawnFn>(kSpawn)(manager, desc);
}
// The puppet's item with that definition name hash (+0x24 -> def +0x80): held, else the inventory [soldier +0x26B8]
uint8_t* ItemByHash(uint8_t* s, uint32_t hash) {
    auto match = [hash](uint8_t* item) {
        const uint8_t* def = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
        return def && *reinterpret_cast<const uint32_t*>(def + 0x80) == hash;
    };
    if (uint8_t* held = *reinterpret_cast<uint8_t**>(s + 0x26B0); match(held)) return held;
    if (uint8_t** inv = *reinterpret_cast<uint8_t***>(s + 0x26B8))
        for (int k = 0; k < 25; ++k)
            if (match(inv[k])) return inv[k];
    return nullptr;
}

// ---- vehicles ----
// Owner = the owner of the driver (seat 0 of the occupant array [veh +0x2608]), else the host. Physics vehicles (+0x2CD8
// bit 3) not owned here are puppets: their update (slot 0) is skipped, the owner's records ('L' 8, every 50 ms: position
// = render instance +0xF4 cm, rotation FUN_004c9a40 degrees, chassis velocities, health, gun 0's current turret angles)
// place them through the game's own placement (snapped beyond 50 cm, else steered by velocity), the turret through its
// slew. Kinematic vehicles still run on every PC. Damage (slot 3) follows the soldiers' rule ("EA V").
bool IsVehicle(const uint8_t* o) { return o && *reinterpret_cast<const uint32_t*>(o) == kVehicleVtable; }
uint8_t* Driver(const uint8_t* v) {
    uint8_t** seats = *reinterpret_cast<uint8_t** const*>(v + 0x2608);
    return seats ? seats[0] : nullptr;
}
bool Physics(const uint8_t* v) { return (*reinterpret_cast<const uint32_t*>(v + 0x2CD8) & 8) != 0; }
bool Destroyed(const uint8_t* v) { return (*reinterpret_cast<const uint32_t*>(v + 0xC) & 0x400) != 0; }
int VehicleOwner(const uint8_t* v) {
    const uint8_t* d = Driver(v);
    return IsSoldier(d) && !Destroyed(v) ? OwnerOf(d) : 0;
}
bool VehiclePuppet(const uint8_t* v) { return g_on && IsVehicle(v) && Physics(v) && !Mine(VehicleOwner(v)); }
uint8_t* Chassis(const uint8_t* v) {   // Karma body of a wheeled (type 0) / tracked (1) vehicle
    const uint8_t* unit = *reinterpret_cast<uint8_t* const*>(v + 0x24);
    const uint8_t* a = unit ? *reinterpret_cast<uint8_t* const*>(unit + 0x80) : nullptr;
    const uint8_t* b = a ? *reinterpret_cast<uint8_t* const*>(a + 0x44) : nullptr;
    const uint32_t type = b ? *reinterpret_cast<const uint32_t*>(b + 0x24) : 99;
    const uint8_t* ctrl = type == 0   ? *reinterpret_cast<uint8_t* const*>(v + 0x2C18)
                          : type == 1 ? *reinterpret_cast<uint8_t* const*>(v + 0x2C1C)
                                      : nullptr;
    return ctrl ? *reinterpret_cast<uint8_t* const*>(ctrl + (type == 0 ? 0xDE8 : 0xEB8)) : nullptr;
}
const float* VehiclePos(const uint8_t* v) {
    const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(v + 0x2C);
    return reinterpret_cast<const float*>(inst + (*reinterpret_cast<const uint32_t*>(inst + 0x1C8) ? 0x13C : 0xF4));
}
#pragma pack(push, 1)
struct VRec {
    uint16_t id;
    float pos[3], rot[3], lin[3], ang[3], health, turret[2];
};
#pragma pack(pop)
struct VPuppet {
    VRec rec{};
    uint32_t seq = 0;
    bool fresh = false;
};
std::map<uint16_t, VPuppet> g_vehicles;
uint32_t g_vehicleRecords = 0, g_vehiclesApplied = 0;

void __fastcall VehicleUpdate(uint8_t* v) {
    if (!VehiclePuppet(v)) return reinterpret_cast<void(__fastcall*)(void*)>(kVehUpdate)(v);
    // health gone (the owner's PC destroyed it - "EA VX" / a record): the vehicle's own update blows it up (wreck,
    // fire, crew out); a puppet never ran it, so the joiner's tank took the damage and stood on (2026-10-22)
    if (*reinterpret_cast<const float*>(v + 0x54) <= 0.0f && !Destroyed(v))
        return reinterpret_cast<void(__fastcall*)(void*)>(kVehUpdate)(v);
    auto it = g_vehicles.find(static_cast<uint16_t>(IdOf(v)));
    if (it != g_vehicles.end() && *reinterpret_cast<uint8_t* const*>(v + 0x260C)) {   // turret: gun 0's target
        *reinterpret_cast<float*>(v + 0x29F8) = it->second.rec.turret[0];
        *reinterpret_cast<float*>(v + 0x2A0C) = it->second.rec.turret[1];
        reinterpret_cast<void(__fastcall*)(void*)>(kTurretSlew)(v);
    }
}

using VehDamageFn = void(__thiscall*)(void*, float*, float, void*);
void __fastcall VehicleDamage(uint8_t* v, void*, float* dir, float amount, uint8_t* attacker) {
    if (!g_on || g_delivering) return reinterpret_cast<VehDamageFn>(kVehDamage)(v, dir, amount, attacker);
    const bool mine = Mine(VehicleOwner(v));
    const bool counts = IsSoldier(attacker)   ? Local(attacker)
                        : IsVehicle(attacker) ? Mine(VehicleOwner(attacker))
                                              : mine;
    if (!counts) return;
    if (mine) return reinterpret_cast<VehDamageFn>(kVehDamage)(v, dir, amount, attacker);
    float d[3] = {};
    if (dir) memcpy(d, dir, 12);
    uint32_t b[4];
    memcpy(b, &amount, 4);
    memcpy(b + 1, d, 12);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "EA V %u %08X %d %08X %08X %08X", IdOf(v), b[0],
             attacker ? static_cast<int>(IdOf(attacker)) : -1, b[1], b[2], b[3]);
    SendEA(cmd);
}

void SendVehicleRecords() {
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    std::vector<VRec> recs;
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* v = table[id];
        if (!IsVehicle(v) || !Physics(v) || !Mine(VehicleOwner(v)) || !(*reinterpret_cast<uint32_t*>(v + 0xC) & 1))
            continue;
        VRec r{static_cast<uint16_t>(id)};
        memcpy(r.pos, VehiclePos(v), 12);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(v, r.rot);
        if (uint8_t* body = Chassis(v)) {
            reinterpret_cast<void(__stdcall*)(void*, float*)>(kGetLinVel)(body, r.lin);
            reinterpret_cast<void(__stdcall*)(void*, float*)>(kGetAngVel)(body, r.ang);
        }
        r.health = *reinterpret_cast<float*>(v + 0x54);
        r.turret[0] = *reinterpret_cast<float*>(v + 0x2A70);
        r.turret[1] = *reinterpret_cast<float*>(v + 0x2A84);
        recs.push_back(r);
    }
    constexpr size_t kPer = 18;   // 18 x 70 + 8 bytes
    for (size_t at = 0; at < recs.size(); at += kPer) {
        const size_t n = std::min(kPer, recs.size() - at);
        std::vector<uint8_t> d = {'L', 8, static_cast<uint8_t>(n), static_cast<uint8_t>(g_local), 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(&d[4], &g_seq, 4);
        memcpy(&d[8], &g_epoch, 4);
        const auto* b = reinterpret_cast<const uint8_t*>(recs.data() + at);
        d.insert(d.end(), b, b + n * sizeof(VRec));
        net::SendRaw(d.data(), static_cast<int>(d.size()));
    }
    g_vehicleRecords += static_cast<uint32_t>(recs.size());
}

// Destroyed vehicles: the records only cover physics vehicles in play, so a kinematic one or one leaving play when it
// blew up never told the others (the bridge tank in Mission 1 burned on the host, stood on the joiner - 2026-10-22).
// The owner looks over its vehicles a few times a second and sends "EA VX <id>" once for each that is destroyed; the
// others destroy it through the game's damage path.
std::map<uint32_t, bool> g_vxSent;
void WatchDestroyedVehicles() {
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* v = table[id];
        if (!IsVehicle(v) || !Mine(VehicleOwner(v)) || g_vxSent.count(id)) continue;
        if (!Destroyed(v) && *reinterpret_cast<float*>(v + 0x54) > 0.0f) continue;
        g_vxSent[id] = true;
        SendEA("EA VX " + std::to_string(id));
    }
}
void DestroyVehicle(uint32_t id) {
    uint8_t* v = ById(id);
    if (!IsVehicle(v) || Destroyed(v)) return;
    float dir[3] = {0, 1, 0};
    // the damage path (FUN_0043e1f0) ignores a vehicle whose health is already 0 - the joiner's tank sat at 0 without
    // the destroyed flag (+0xC 0x400): a sliver of health first so it goes through the game's destruction
    float& health = *reinterpret_cast<float*>(v + 0x54);
    const float before = health;
    if (health <= 0.0f) health = 1.0f;
    g_delivering = true;
    reinterpret_cast<VehDamageFn>(kVehDamage)(v, dir, health + 100000.0f, nullptr);
    g_delivering = false;
    char line[160];
    snprintf(line, sizeof line, "Authority: vehicle %u destroyed on its owner's PC - destroyed here too (health %.1f before, %s, %s)",
             id, before, VehiclePuppet(v) ? "puppet" : "runs here", Destroyed(v) ? "wrecked" : "NOT wrecked");
    features::OnlineLogLine(line);
}

using SetVecFn = void(__stdcall*)(void*, float, float, float);
void ApplyVehicle(uint8_t* v, const VRec& r) {
    float& health = *reinterpret_cast<float*>(v + 0x54);
    if (r.health <= 0.0f && !Destroyed(v)) {   // destroyed on the owner's PC: through the game's own path here
        float dir[3] = {0, 1, 0};
        if (health <= 0.0f) health = 1.0f;   // (the damage path skips a vehicle already at 0)
        g_delivering = true;
        reinterpret_cast<VehDamageFn>(kVehDamage)(v, dir, health + 100000.0f, nullptr);
        g_delivering = false;
        return;
    }
    if (Destroyed(v)) return;
    health = r.health;
    const float* cur = VehiclePos(v);
    const float dx = r.pos[0] - cur[0], dy = r.pos[1] - cur[1], dz = r.pos[2] - cur[2];
    float pos[3], rot[3];
    memcpy(pos, r.pos, 12);
    memcpy(rot, r.rot, 12);
    uint8_t* body = Chassis(v);
    // facing: steering by velocity alone let the heading drift (Mission 1's truck drove crabwise on the joiner,
    // 2026-10-22) - more than 5 degrees off on any axis (degrees, FUN_004c9a40) places it like a far position
    float now[3] = {};
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(v, now);
    float turn = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float d = std::fmod(std::fabs(r.rot[k] - now[k]), 360.0f);
        turn = std::max(turn, d > 180.0f ? 360.0f - d : d);
    }
    if (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f || turn > 5.0f || !body) {
        reinterpret_cast<void(__thiscall*)(void*, float*, float*)>(kVehPlace)(v, pos, rot);
        if (body) {
            reinterpret_cast<SetVecFn>(kSetLinVel)(body, r.lin[0], r.lin[1], r.lin[2]);
            reinterpret_cast<SetVecFn>(kSetAngVel)(body, r.ang[0], r.ang[1], r.ang[2]);
        }
    } else {   // close: its own physics carries on, steered towards the owner's position (cm -> m, over ~0.5 s)
        reinterpret_cast<SetVecFn>(kSetLinVel)(body, r.lin[0] + dx * 0.02f, r.lin[1] + dy * 0.02f, r.lin[2] + dz * 0.02f);
        reinterpret_cast<SetVecFn>(kSetAngVel)(body, r.ang[0], r.ang[1], r.ang[2]);
    }
    ++g_vehiclesApplied;
}

// Mounting / dismounting: an owned soldier's vehicle (+0x267C) changing is sent ("EA MV soldier vehicle seat",
// "EA DV soldier"); the other PCs mount / dismount that soldier the way the game does (seat place FUN_00442300 + mount
// FUN_00433f00, dismount FUN_0042e850).
std::map<uint32_t, uint32_t> g_mounted;   // owned soldier id -> vehicle id (0xFFFFFFFF none)
int SeatOf(const uint8_t* v, const uint8_t* s) {
    uint8_t** seats = *reinterpret_cast<uint8_t** const*>(v + 0x2608);
    const uint8_t* unit = *reinterpret_cast<uint8_t* const*>(v + 0x24);
    const uint8_t* a = unit ? *reinterpret_cast<uint8_t* const*>(unit + 0x80) : nullptr;
    const int n = a ? a[0x40] : 0;
    for (int k = 0; seats && k < n; ++k)
        if (seats[k] == s) return k;
    return -1;
}
void WatchMounts() {
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* s = table[id];
        if (!IsSoldier(s) || !Local(s)) continue;
        const uint8_t* v = *reinterpret_cast<uint8_t* const*>(s + 0x267C);
        const uint32_t vid = IsVehicle(v) ? IdOf(v) : 0xFFFFFFFF;
        auto [it, added] = g_mounted.emplace(id, vid);
        if (added || it->second == vid) continue;
        it->second = vid;
        if (vid != 0xFFFFFFFF)
            SendEA("EA MV " + std::to_string(id) + " " + std::to_string(vid) + " " +
                             std::to_string(SeatOf(v, s)));
        else
            SendEA("EA DV " + std::to_string(id));
    }
}

// ---- mission script ----
struct ScriptObject { void* fn[6]; };
ScriptObject g_script{}, *g_original = nullptr;
using MessageFn = int(__cdecl*)(int, uint32_t*);
bool IsQuery(int id) {
    switch (id) {
    case 0x0F: case 0x14: case 0x1A: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C:
    case 0x2D: case 0x2E: case 0x33: case 0x37: case 0x38: case 0x3D: case 0x43: case 0x44: case 0x45: case 0x46:
    case 0x47: case 0x49: case 0x4C: case 0x4E: case 0x4F: case 0x53:
        return true;
    }
    return false;
}
void CopyParam(uint32_t* words, const uint32_t* param) {
    __try {
        memcpy(words, param, kMessageWords * 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// A joiner's own soldier did it (picked up / dropped an item, moved a switch, unlocked, planted, called an air strike,
// a custom action such as downloading data): only that PC saw it, so the message goes to the host's mission (which
// sends it back to everyone as "EA M"). 2-PC test 2026-10-16, Mission 3: the joiner's unlock / pick-up never reached
// the host - "tunnel didn't open for host, client didn't get the objective complete".
bool JoinerAction(int id, const uint32_t* words) {
    switch (id) {
    case 0x02: case 0x03: case 0x0D: case 0x0E: case 0x13: case 0x17: case 0x1E: case 0x4A:
        break;
    default:
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (words[k] == 0 || words[k] > 0xFFFF) continue;
        uint8_t* s = ById(words[k]);
        if (IsSoldier(s) && Local(s)) return true;
    }
    return false;
}
std::string MessageCommand(const char* kind, int id, bool has, const uint32_t* words) {
    std::string cmd = std::string(kind) + " " + std::to_string(id) + (has ? " 1" : " 0");
    for (int k = 0; k < kMessageWords; ++k) {
        char hex[12];
        snprintf(hex, sizeof hex, " %08X", words[k]);
        cmd += hex;
    }
    return cmd;
}
uint32_t g_messagesForwarded = 0;

// ---- doors ----
// Every PC runs its own copy of the doors; a door opened / closed by this PC's soldier (or its proximity check, a
// switch, the host's script) is sent ("EA DR door action") and the others move theirs the same way. Only real changes
// are sent and applied, so a door both PCs open themselves isn't commanded twice.
void* g_doorCommand = nullptr;   // trampoline to the original
uint32_t g_doorsSent = 0, g_doorsApplied = 0;
void __fastcall DoorCommand(uint8_t* d, void*, int action) {
    const bool open = (*reinterpret_cast<uint32_t*>(d + 0x54) & 2) != 0;
    reinterpret_cast<void(__thiscall*)(void*, int)>(g_doorCommand)(d, action);
    if (!g_on || g_delivering || (action != 0 && action != 1) || (action == 1) == open) return;
    if (((*reinterpret_cast<uint32_t*>(d + 0x54) & 2) != 0) == open) return;   // refused (moving / blocked)
    char cmd[48];
    snprintf(cmd, sizeof cmd, "EA DR %u %d", IdOf(d), action);
    SendEA(cmd);
    ++g_doorsSent;
}

int __cdecl ScriptMessage(int id, uint32_t* param) {
    const auto original = reinterpret_cast<MessageFn>(g_original ? g_original->fn[2] : nullptr);
    if (!original) return 1;
    if (!g_on || g_delivering || IsQuery(id) || id < 0 || id > 0xFFFF) return original(id, param);
    uint32_t words[kMessageWords] = {};
    if (param) CopyParam(words, param);
    if (!g_host) {
        if (JoinerAction(id, words)) {   // to the host; delivered here when it comes back as "EA M"
            SendEA(MessageCommand("EA MJ", id, param != nullptr, words));
            ++g_messagesForwarded;
        } else {
            ++g_messagesDropped;
        }
        return 1;
    }
    std::string cmd = "EA M " + std::to_string(id) + (param ? " 1" : " 0");
    for (uint32_t w : words) {
        char hex[12];
        snprintf(hex, sizeof hex, " %08X", w);
        cmd += hex;
    }
    SendEA(cmd);
    ++g_messagesSent;
    return original(id, param);
}
void HookScript() {
    auto* now = *reinterpret_cast<ScriptObject**>(kScriptObject);
    if (!now || now == &g_script) return;
    g_original = now;
    g_script = *now;
    g_script.fn[2] = reinterpret_cast<void*>(&ScriptMessage);
    *reinterpret_cast<ScriptObject**>(kScriptObject) = &g_script;
}
void UnhookScript() {
    if (*reinterpret_cast<ScriptObject**>(kScriptObject) == &g_script)
        *reinterpret_cast<ScriptObject**>(kScriptObject) = g_original;
    g_original = nullptr;
}

void HandleEvent(const std::string& e) {
    int voter = -1;
    unsigned votes = 0;
    unsigned vxId = 0;
    if (sscanf_s(e.c_str(), "EA VX %u", &vxId) == 1) {   // a vehicle destroyed on its owner's PC
        DestroyVehicle(vxId);
        return;
    }
    unsigned puSoldier = 0, puItem = 0;
    if (sscanf_s(e.c_str(), "EA PU %u %u", &puSoldier, &puItem) == 2) {   // another PC's soldier picked an item up
        ReplayPickUp(puSoldier, puItem);
        return;
    }
    if (sscanf_s(e.c_str(), "EA CSV %d", &voter) == 1) {   // a joiner's player voted to skip
        if (g_host) HostVote(voter, "another PC");
        return;
    }
    if (e == "EA CSR") {   // (an older joiner's plain skip request: the votes of all its players)
        if (g_host)
            for (int k = 0; k < 32; ++k)
                if (((VotersMask() & ~g_mask) >> k) & 1) HostVote(k, "another PC");
        return;
    }
    if (sscanf_s(e.c_str(), "EA CSN %X", &votes) == 1) {   // the host's count of the votes (indicator)
        if (!g_host) g_votes = votes | (InCutscene() ? (g_votes & g_mask) : 0);
        return;
    }
    if (e == "EA RR") {   // a joiner chose RESTART on MISSION FAILED: the host's screen restarts on its next update
        if (g_host) {
            if (!g_restartRequestAt) {
                const uint32_t state = *reinterpret_cast<const uint32_t*>(kFrontEndState);
                const uint8_t* screen = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(kScreenOf)(state);
                char line[128];
                snprintf(line, sizeof line, "Online: a player asks to restart (this PC: front-end state 0x%X, screen %08X)",
                         state, screen ? *reinterpret_cast<const uint32_t*>(screen) : 0);
                features::OnlineLogLine(line);
            }
            g_restartRequestAt = GetTickCount();
        }
        return;
    }
    if (e == "EA CS") {   // the host skipped the cutscene
        if (!g_host && InCutscene()) SkipNow("Authority: cutscene skipped by the host"), ++g_skipsTaken;
        return;
    }
    unsigned id = 0, bits = 0;
    int attacker = -1;
    if (sscanf_s(e.c_str(), "EA D %u %X %d", &id, &bits, &attacker) == 3) {
        uint8_t* s = ById(id);
        if (!IsSoldier(s) || !Local(s)) return;
        float amount;
        memcpy(&amount, &bits, 4);
        g_delivering = true;
        reinterpret_cast<DamageFn>(kTakeDamage)(s, amount, attacker >= 0 ? ById(attacker) : nullptr);
        g_delivering = false;
        ++g_damageApplied;
        return;
    }
    if (sscanf_s(e.c_str(), "EA H %u %X", &id, &bits) == 2) {
        uint8_t* s = ById(id);
        if (!IsSoldier(s) || !Local(s)) return;
        float amount;
        memcpy(&amount, &bits, 4);
        g_delivering = true;
        reinterpret_cast<HealFn>(kHeal)(s, amount);
        g_delivering = false;
        return;
    }
    if (sscanf_s(e.c_str(), "EA MK %u %X", &id, &bits) == 2) {   // another PC's medikit on this PC's soldier
        uint8_t* s = ById(id);
        if (!IsSoldier(s) || !Local(s)) return;
        float amount;
        memcpy(&amount, &bits, 4);
        if (amount > 0.0f && amount < 10000.0f) ApplyMedikit(s, amount);
        return;
    }
    unsigned player = 0;
    if (sscanf_s(e.c_str(), "EA S %u %u", &player, &id) == 2) {   // another player now controls soldier id
        uint8_t* s = ById(id);
        if (player >= static_cast<unsigned>(g_players) || player >= 4 || !IsSoldier(s)) return;
        auto* block = reinterpret_cast<uint8_t*>(kBlocks + player * kBlockSize);
        uint8_t* old = *reinterpret_cast<uint8_t**>(block + 0x310);
        if (old == s) return;
        if (Mine(static_cast<int>(player)) && old) return;   // this PC's own player: only filled when empty
        if (old) reinterpret_cast<void(__thiscall*)(void*)>(kRelease)(old);
        reinterpret_cast<void(__thiscall*)(void*, int)>(kSetMode)(block, 2);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kTakeSoldier)(block, s);
        reinterpret_cast<void(__thiscall*)(void*)>(kActivate)(s);
        reinterpret_cast<void(__cdecl*)()>(kHudRefresh)();
        return;
    }
    {
        unsigned hash = 0, mode = 0, f[6];
        if (sscanf_s(e.c_str(), "EA P %u %X %u %X %X %X %X %X %X", &id, &hash, &mode, &f[0], &f[1], &f[2], &f[3], &f[4],
                     &f[5]) == 9) {
            uint8_t* s = ById(id);
            if (!IsPuppet(s)) return;
            g_spawn.pending = true;
            memcpy(g_spawn.pos, f, 12);
            memcpy(&g_spawn.speed, f + 3, 12);
            uint8_t* item = ItemByHash(s, hash);
            const uint32_t cls = item ? *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(item + 0x24) + 0xC0) : 0;
            if (cls == 7 || cls == 0xB || cls == 0xD) {
                // a grenade (class 7 / 0xB): thrown now through the game's throw with the owner's values
                reinterpret_cast<void(__thiscall*)(void*, int)>(kSetRounds)(item, 2);
                g_delivering = true;
                reinterpret_cast<int(__thiscall*)(void*, void*)>(kThrow)(item, s);
                g_delivering = false;
                g_spawn.pending = false;
            } else if (uint8_t* weapon = *reinterpret_cast<uint8_t**>(s + 0x26B0);
                       weapon && *reinterpret_cast<uint32_t*>(weapon + 0x5C) != mode && mode) {
                // rockets / M203: the shot itself comes with "EA F" right after - the fire mode first (M203 = 0x100)
                reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kSetFireMode)(weapon, mode, 0);
            }
            return;
        }
    }
    {
        unsigned vid = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        int att = -1, seat = -1;
        if (sscanf_s(e.c_str(), "EA V %u %X %d %X %X %X", &vid, &b0, &att, &b1, &b2, &b3) == 6) {
            uint8_t* v = ById(vid);
            if (!IsVehicle(v) || !Mine(VehicleOwner(v))) return;
            float amount, dir[3];
            memcpy(&amount, &b0, 4);
            memcpy(&dir[0], &b1, 4);
            memcpy(&dir[1], &b2, 4);
            memcpy(&dir[2], &b3, 4);
            g_delivering = true;
            reinterpret_cast<VehDamageFn>(kVehDamage)(v, dir, amount, att >= 0 ? ById(att) : nullptr);
            g_delivering = false;
            return;
        }
        if (sscanf_s(e.c_str(), "EA MV %u %u %d", &id, &vid, &seat) == 3) {
            uint8_t* s = ById(id);
            uint8_t* v = ById(vid);
            if (!IsSoldier(s) || Local(s) || !IsVehicle(v) || seat < 0) return;
            if (*reinterpret_cast<uint8_t**>(s + 0x267C) == v) return;
            if (*reinterpret_cast<uint8_t**>(s + 0x267C))
                reinterpret_cast<int(__thiscall*)(void*, int, int)>(kDismount)(s, 0, 0);
            int a = 0, b = 0;
            if (reinterpret_cast<int(__thiscall*)(void*, int, int*, int*)>(kSeatPlace)(v, seat, &a, &b))
                reinterpret_cast<void(__thiscall*)(void*, void*, int, int, int)>(kMount)(s, v, a, b, 0);
            return;
        }
        if (sscanf_s(e.c_str(), "EA DV %u", &id) == 1) {
            uint8_t* s = ById(id);
            if (IsSoldier(s) && !Local(s) && *reinterpret_cast<uint8_t**>(s + 0x267C))
                reinterpret_cast<int(__thiscall*)(void*, int, int)>(kDismount)(s, 0, 0);
            return;
        }
    }
    if (sscanf_s(e.c_str(), "EA F %u", &id) == 1) {
        uint8_t* s = ById(id);
        if (!IsPuppet(s)) return;
        if (void* weapon = *reinterpret_cast<void**>(s + 0x26B0)) {
            g_delivering = true;   // (not sent on again)
            reinterpret_cast<TriggerFn>(kTrigger)(weapon, s, 0, 0, 0);
            g_delivering = false;
            g_spawn.pending = false;   // (an "EA P" whose shot didn't spawn must not move the next one)
            ++g_firesReplayed;
        }
        return;
    }
    if (e.compare(0, 6, "EA MJ ") == 0 && g_host) {   // a joiner's own action: into the mission here + to everyone
        unsigned msg = 0, has = 0;
        uint32_t w[kMessageWords] = {};
        if (sscanf_s(e.c_str(), "EA MJ %u %u %X %X %X %X %X %X %X %X", &msg, &has, &w[0], &w[1], &w[2], &w[3], &w[4],
                     &w[5], &w[6], &w[7]) != 10)
            return;
        HookScript();
        if (*reinterpret_cast<ScriptObject**>(kScriptObject) != &g_script) return;
        ScriptMessage(static_cast<int>(msg), has ? w : nullptr);
        ++g_messagesForwarded;
        return;
    }
    if (sscanf_s(e.c_str(), "EA DR %u %u", &id, &bits) == 2) {   // a door moved on another PC
        uint8_t* d = ById(id);
        if (!d || IsSoldier(d) || IsVehicle(d)) return;
        const bool open = (*reinterpret_cast<uint32_t*>(d + 0x54) & 2) != 0;
        if ((bits == 1) == open) return;   // already so here
        g_delivering = true;
        reinterpret_cast<void(__thiscall*)(void*, int)>(g_doorCommand)(d, static_cast<int>(bits));
        g_delivering = false;
        ++g_doorsApplied;
        return;
    }
    if (e.compare(0, 5, "EA M ") == 0 && !g_host) {
        unsigned msg = 0, has = 0;
        uint32_t w[kMessageWords] = {};
        if (sscanf_s(e.c_str(), "EA M %u %u %X %X %X %X %X %X %X %X", &msg, &has, &w[0], &w[1], &w[2], &w[3], &w[4],
                     &w[5], &w[6], &w[7]) != 10)
            return;
        HookScript();
        if (!g_original || !g_original->fn[2]) return;
        // 0x07 destructible destroyed (object, attacker): the mission's handler looks at the object (Mission 1's bridge
        // tank -> the bridge camera cut) - it must be destroyed here first, else the joiner got no cut (2026-10-22)
        if (msg == 0x07 && has) DestroyVehicle(w[0]);
        g_delivering = true;
        reinterpret_cast<MessageFn>(g_original->fn[2])(static_cast<int>(msg), has ? w : nullptr);
        g_delivering = false;
        ++g_messagesDelivered;
    }
}

// ---- records ----
void SendRecords() {
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    std::vector<Rec> recs;
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* s = table[id];
        if (!IsSoldier(s) || !Local(s) || Mounted(s)) continue;
        Rec r = puppetsync::Encode(static_cast<uint16_t>(id), ReadView(s));
        memcpy(r.pos, s + 0x30, 12);
        float rot[3] = {};
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(s, rot);
        r.heading = rot[1];
        memcpy(r.aim, s + 0x328, 12);
        r.health = *reinterpret_cast<float*>(s + 0x54);
        r.pending = *reinterpret_cast<float*>(s + 0x2B84);
        recs.push_back(r);
    }
    ++g_seq;
    for (const auto& d : puppetsync::Packets(recs, g_local, g_seq, g_epoch))
        net::SendRaw(d.data(), static_cast<int>(d.size()));
    g_sentRecords += static_cast<uint32_t>(recs.size());
}
}  // namespace

bool features::AuthorityOn() { return g_on; }
void features::AuthorityDevSkipVote(int player) { LocalVote(player); }   // dev: as that player's skip press
bool features::AuthorityPuppet(const uint8_t* soldier) { return IsPuppet(soldier); }

void features::AuthorityStart(bool host, int players) {
    patch::OwnerScope owner(pse_host::Find("dsfix.authority"));  // lazy hooks: owned by this feature's PSE plugin
    if (!g_installed) {
        bool ok = patch::HookCall(kBrainCall, reinterpret_cast<const void*>(&Brain), kBrain) &&
                  patch::HookCall(kActionCall, reinterpret_cast<const void*>(&Action), kAction) &&
                  patch::HookCall(kMoverCall, reinterpret_cast<const void*>(&Mover), kMover) &&
                  patch::HookCall(kHoldMachineCall, reinterpret_cast<const void*>(&HoldMachine), kHoldMachine) &&
                  patch::HookCall(kTriggerCall, reinterpret_cast<const void*>(&Trigger), kTrigger) &&
                  patch::HookCall(kCancelCalls[0], reinterpret_cast<const void*>(&CancelPressed), kActionPressed) &&
                  patch::HookCall(kCancelCalls[1], reinterpret_cast<const void*>(&CancelPressed), kActionPressed) &&
                  patch::HookCall(kFailedListCall, reinterpret_cast<const void*>(&FailedListInput), kListInput) &&
                  patch::HookCall(kThrowSpawnCall, reinterpret_cast<const void*>(&SpawnProjectile), kSpawn) &&
                  patch::HookCall(kFireSpawnCall, reinterpret_cast<const void*>(&SpawnProjectile), kSpawn);
        const uint32_t original = kTakeDamage, ours = reinterpret_cast<uint32_t>(&TakeDamage);
        const uint32_t kb = kKnockback, ourKb = reinterpret_cast<uint32_t>(&Knockback);
        if (patch::Matches(kKnockbackSlot, &kb, 4)) patch::Write(kKnockbackSlot, &ourKb, 4);
        const uint32_t vu = kVehUpdate, ourVu = reinterpret_cast<uint32_t>(&VehicleUpdate);
        const uint32_t vd = kVehDamage, ourVd = reinterpret_cast<uint32_t>(&VehicleDamage);
        if (patch::Matches(kVehUpdateSlot, &vu, 4) && patch::Matches(kVehDamageSlot, &vd, 4)) {
            patch::Write(kVehUpdateSlot, &ourVu, 4);
            patch::Write(kVehDamageSlot, &ourVd, 4);
        } else {
            features::OnlineLogLine("[fail] Authority: vehicle slots not hooked - vehicles run on every PC");
        }
        if (patch::Matches(kDoorCommand, kDoorEntry, sizeof kDoorEntry)) {
            auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (t) {
                memcpy(t, kDoorEntry, sizeof kDoorEntry);
                t[sizeof kDoorEntry] = 0xE9;
                const int32_t rel = static_cast<int32_t>(kDoorCommand + sizeof kDoorEntry -
                                                         (reinterpret_cast<uint32_t>(t) + sizeof kDoorEntry + 5));
                memcpy(t + sizeof kDoorEntry + 1, &rel, 4);
                g_doorCommand = t;
                patch::WriteJump(kDoorCommand, reinterpret_cast<const void*>(&DoorCommand));
            }
        }
        if (!g_doorCommand) features::OnlineLogLine("[fail] Authority: door hook not installed - doors per PC");
        if (!HookSetMoveState())
            features::OnlineLogLine("[fail] Authority: movement state hook not installed - puppets may slide");
        const uint32_t heal = kHeal, ourHeal = reinterpret_cast<uint32_t>(&Heal);
        if (ok && patch::Matches(kTakeDamageSlot, &original, 4) && patch::Matches(kHealSlot, &heal, 4)) {
            patch::Write(kTakeDamageSlot, &ours, 4);
            patch::Write(kHealSlot, &ourHeal, 4);
        } else {
            ok = false;
        }
        if (!ok) {
            features::OnlineLogLine("[fail] Authority: hooks not installed - every PC runs every soldier (no sync)");
            return;
        }
        g_installed = true;
        overlay::AddLateDraw(&VoteDraw);
        if (!patch::HookFunction(kPickUp, reinterpret_cast<const void*>(&PickUp), &g_pickUpNext, kPickUpEntry, sizeof kPickUpEntry))
            features::OnlineLogLine("[fail] Authority: pick-up hook not installed - pick-ups stay on each PC");
    }
    g_on = true;
    g_votes = 0;
    g_wasCutscene = false;
    g_vxSent.clear();
    g_host = host;
    g_players = players;
    g_mask = features::LobbyLocalMask();
    g_local = features::LobbyFirstLocal();
    g_seq = 0;
    g_epoch = features::LockstepSeed();
    g_lastRecords = 0;
    for (uint32_t& b : g_blockSent) b = 0xFFFFFFFF;
    g_puppets.clear();
    g_events.clear();
    g_vehicles.clear();
    g_mounted.clear();
    g_sentRecords = g_appliedRecords = g_damageSent = g_damageApplied = g_damageIgnored = g_firesSent = g_firesReplayed =
        g_messagesSent = g_messagesDelivered = g_messagesDropped = g_messagesForwarded = g_doorsSent = g_doorsApplied = 0;
    char line[160];
    snprintf(line, sizeof line, "Authority: players mask %X of %d - this PC runs its own soldiers%s, the others follow "
             "their owners' records (mission start %08X)", g_mask, players, host ? ", the AI and the mission" : "", g_epoch);
    features::OnlineLogLine(line);
}

void features::AuthorityStop() {
    if (!g_on) return;
    char line[400];
    snprintf(line, sizeof line, "Authority: stopped - records sent %u / applied %u, damage sent %u / applied %u / "
             "ignored %u, shots sent %u / replayed %u, script messages sent %u / delivered %u / dropped %u / joiner "
             "actions %u, doors sent %u / applied %u, pick-ups sent %u / applied %u",
             g_sentRecords, g_appliedRecords, g_damageSent, g_damageApplied, g_damageIgnored, g_firesSent,
             g_firesReplayed, g_messagesSent, g_messagesDelivered, g_messagesDropped, g_messagesForwarded, g_doorsSent,
             g_doorsApplied, g_pickUpsSent, g_pickUpsApplied);
    features::OnlineLogLine(line);
    UnhookScript();
    g_on = false;
}

// (the old lockstep event stream: nothing travels in the inputs any more)
std::string features::AuthorityTakeEvents() { return std::string(); }
void features::AuthorityDeliver(const std::string&) {}

void features::AuthorityCommand(const std::string& text) {
    unsigned epoch = 0;
    if (!g_on || sscanf_s(text.c_str(), "EA #%X ", &epoch) != 1 || text.size() < 13) return;
    if (epoch != g_epoch) {
        ++g_otherEpochs, g_lastOtherEpoch = epoch;
        return;
    }
    g_events.push_back("EA " + text.substr(13));
}

void features::AuthorityReceive(const std::vector<uint8_t>& d) {   // 'L' 10: another PC's records
    if (!g_on) return;
    const uint32_t before = g_receiver.otherEpochs;
    g_receiver.Take(d.data(), d.size(), g_epoch);
    if (g_receiver.otherEpochs != before) ++g_otherEpochs, g_lastOtherEpoch = g_receiver.lastOtherEpoch;
}

void features::AuthorityReceiveVehicles(const std::vector<uint8_t>& d) {   // 'L' 8: another PC's vehicle records
    if (!g_on || d.size() < 12) return;
    const int n = d[2];
    uint32_t seq, epoch;
    memcpy(&seq, &d[4], 4);
    memcpy(&epoch, &d[8], 4);
    if (epoch != g_epoch) {   // another mission start's
        ++g_otherEpochs, g_lastOtherEpoch = epoch;
        return;
    }
    if (d.size() < 12 + n * sizeof(VRec)) return;
    const auto* r = reinterpret_cast<const VRec*>(&d[12]);
    for (int k = 0; k < n; ++k) {
        VPuppet& p = g_vehicles[r[k].id];
        if (seq < p.seq) continue;
        p.rec = r[k];
        p.seq = seq;
        p.fresh = true;
    }
}

// Every frame of the mission (lockstep.cpp, before the game update).
// ---- state dump (every build, online) ----
// DesertStormFix-sync.csv next to the exe: once a second every soldier and vehicle as this PC sees it - position,
// facing, aim, movement / stance / hold, held item, both animation channels, health, owner and whether it is this PC's
// or a puppet - so the host's and a joiner's files can be laid side by side afterwards (user, 2026-10-22: the truck,
// aiming and some AI looked different on the two PCs). Rows carry UTC milliseconds (both PCs' clocks, usually within
// tens of ms) and milliseconds since the mission started on this PC; a "#" line marks each mission start (level, mission
// seed = the same on every PC, this PC's number, host / joiner). Columns: kDumpHeader. Capped at 256 MB.
constexpr char kDumpHeader[] =
    "utc_ms,mission_ms,kind,id,owner_pc,here,team,in_play,health,x,y,z,rot_x,rot_y,rot_z,aim_pitch,aim_yaw,move,stance,"
    "hold,item,anim0,anim0_t,anim1,anim1_t,vehicle_or_driver,target,asleep,vx,vy,vz,turret0,turret1,flags\n";
FILE* g_dump = nullptr;
uint32_t g_dumpEpoch = 0;
DWORD g_dumpStart = 0;
long long g_dumpBytes = 0;

struct DumpRow {
    char kind = 'S';
    float pos[3] = {}, rot[3] = {}, aim[2] = {}, vel[3] = {}, turret[2] = {}, health = 0, t[2] = {};
    int32_t team = 0, move = 0, stance = 0, hold = 0, asleep = 0;
    uint32_t inPlay = 0, item = 0, anim[2] = {}, other = 0xFFFFFFFF, target = 0xFFFFFFFF, flags = 0;
};
bool ReadSoldierRow(uint8_t* s, DumpRow& r) {   // (guarded: objects may be mid-change)
    __try {
        r.kind = 'S';
        r.flags = *reinterpret_cast<const uint32_t*>(s + 0xC);
        r.inPlay = r.flags & 1;
        r.team = s[0x10];
        r.health = *reinterpret_cast<const float*>(s + 0x54);
        memcpy(r.pos, s + 0x30, 12);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(s, r.rot);
        r.aim[0] = *reinterpret_cast<const float*>(s + 0x328);
        r.aim[1] = *reinterpret_cast<const float*>(s + 0x32C);
        r.move = *reinterpret_cast<const int32_t*>(s + 0x2674);
        r.stance = *reinterpret_cast<const int32_t*>(s + 0x348);
        r.hold = *reinterpret_cast<const int32_t*>(s + 0x2688);
        r.asleep = *reinterpret_cast<const int32_t*>(s + 0x280);
        if (const uint8_t* item = *reinterpret_cast<uint8_t* const*>(s + 0x26B0))
            if (const uint8_t* def = *reinterpret_cast<uint8_t* const*>(item + 0x24))
                r.item = *reinterpret_cast<const uint32_t*>(def + 0x80);
        if (const uint8_t* v = *reinterpret_cast<uint8_t* const*>(s + 0x267C)) r.other = *reinterpret_cast<const uint32_t*>(v + 8);
        if (const uint8_t* tg = *reinterpret_cast<uint8_t* const*>(s + 0x255C)) r.target = *reinterpret_cast<const uint32_t*>(tg + 8);
        if (const uint8_t* inst = *reinterpret_cast<const uint8_t* const*>(s + 0x2C))
            for (int c = 0; c < 2; ++c) {
                const uint8_t* chan = inst + 0x248 + c * 0x34;
                if (*reinterpret_cast<const uint32_t*>(chan + 0x24))
                    if (const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(chan))
                        r.anim[c] = *reinterpret_cast<const uint32_t*>(a);
                r.t[c] = *reinterpret_cast<const float*>(chan + 0xC);
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadVehicleRow(uint8_t* v, DumpRow& r) {
    __try {
        r.kind = 'V';
        r.flags = *reinterpret_cast<const uint32_t*>(v + 0xC);
        r.inPlay = r.flags & 1;
        r.team = v[0x10];
        r.health = *reinterpret_cast<const float*>(v + 0x54);
        memcpy(r.pos, VehiclePos(v), 12);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(v, r.rot);
        if (uint8_t* body = Chassis(v)) reinterpret_cast<void(__stdcall*)(void*, float*)>(kGetLinVel)(body, r.vel);
        r.turret[0] = *reinterpret_cast<const float*>(v + 0x2A70);
        r.turret[1] = *reinterpret_cast<const float*>(v + 0x2A84);
        if (uint8_t** seats = *reinterpret_cast<uint8_t***>(v + 0x2608))
            if (seats[0]) r.other = *reinterpret_cast<const uint32_t*>(seats[0] + 8);   // (driver)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void LevelNow(char* name, size_t size) {   // (the loaded scene, "Mission1.env")
    memset(name, 0, size);
    __try {
        memcpy(name, reinterpret_cast<const void*>(0x606880), size - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    for (size_t i = 0; i < size && name[i]; ++i)
        if (name[i] < 32 || name[i] == ',') name[i] = '_';
}
void DumpState() {
    if (g_dumpBytes > (256ll << 20)) return;
    if (!g_dump) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "DesertStormFix-sync.csv");
        if (fopen_s(&g_dump, path, "w") || !g_dump) return;
        g_dumpBytes += fprintf(g_dump, "%s", kDumpHeader);
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long utc =
        ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000;
    if (g_dumpEpoch != g_epoch) {
        g_dumpEpoch = g_epoch;
        g_dumpStart = GetTickCount();
        char level[64];
        LevelNow(level, sizeof level);
        g_dumpBytes += fprintf(g_dump, "# %llu mission start %08X level %s pc %d %s players %d\n", utc, g_epoch, level,
                               lobbynet::MyPc(), g_host ? "host" : "joiner", g_players);
    }
    const DWORD ms = GetTickCount() - g_dumpStart;
    uint32_t maxId = 0;
    uint8_t** table = Table(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
        uint8_t* o = table[id];
        DumpRow r;
        int owner = 0;
        const char* here = "";
        if (IsSoldier(o)) {
            if (!ReadSoldierRow(o, r)) continue;
            owner = OwnerOf(o);
            here = IsPuppet(o) ? "puppet" : Local(o) ? "own" : "mounted";
        } else if (IsVehicle(o)) {
            if (!ReadVehicleRow(o, r)) continue;
            owner = VehicleOwner(o);
            here = VehiclePuppet(o) ? "puppet" : Mine(owner) ? "own" : "runs";
        } else {
            continue;
        }
        g_dumpBytes += fprintf(
            g_dump,
            "%llu,%lu,%c,%u,%d,%s,%d,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%08X,%08X,%.2f,%08X,%.2f,%d,%d,"
            "%d,%.1f,%.1f,%.1f,%.1f,%.1f,%08X\n",
            utc, ms, r.kind, id, owner + 1, here, r.team, r.inPlay, r.health, r.pos[0], r.pos[1], r.pos[2], r.rot[0],
            r.rot[1], r.rot[2], r.aim[0], r.aim[1], r.move, r.stance, r.hold, r.item, r.anim[0], r.t[0], r.anim[1], r.t[1],
            static_cast<int>(r.other), static_cast<int>(r.target), r.asleep, r.vel[0], r.vel[1], r.vel[2], r.turret[0],
            r.turret[1], r.flags);
    }
    fflush(g_dump);
}

void features::AuthorityTick(uint32_t tick) {
    if (!g_on) return;
    HookScript();
    {   // each block's soldier when it changes (and who changed it: the first code addresses on the stack don't help
        // here - logged once per change with the tick)
        static const uint8_t* seen[4] = {};
        static bool first = true;
        if (tick == 0) first = true;
        for (int p = 0; p < g_players && p < 4; ++p) {
            const uint8_t* bs = *reinterpret_cast<uint8_t* const*>(kBlocks + p * kBlockSize + 0x310);
            if (bs == seen[p] && !first) continue;
            seen[p] = bs;
            char line[160];
            snprintf(line, sizeof line, "Authority: tick %u block %d soldier %s (mode %d)", tick, p + 1,
                     bs ? std::to_string(IdOf(bs)).c_str() : "none",
                     *reinterpret_cast<const int32_t*>(kBlocks + p * kBlockSize + 0x318));
            features::OnlineLogLine(line);
        }
        first = false;
    }
    while (!g_events.empty()) {
        HandleEvent(g_events.front());
        g_events.pop_front();
    }
    VoteFrame();
    {
        static DWORD lastVx = 0;
        if (GetTickCount() - lastVx > 250) lastVx = GetTickCount(), WatchDestroyedVehicles();
    }
    ForwardPendingHeals();
    LogAnims();
    {
        static DWORD lastDump = 0;
        if (GetTickCount() - lastDump >= 1000) lastDump = GetTickCount(), DumpState();
    }
    // A joiner's restart request while this host shows MISSION FAILED: if the screen's own input didn't take it within
    // half a second (2-PC log 2026-10-16: 12 requests, no restart), restart from here (the pause menu's path).
    if (g_host && g_restartRequestAt && GetTickCount() - g_restartRequestAt > 500) {
        const uint32_t state = *reinterpret_cast<const uint32_t*>(kFrontEndState);
        const uint8_t* screen = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t)>(kScreenOf)(state);
        if (screen && *reinterpret_cast<const uint32_t*>(screen) == kFailedScreenVtable) {
            g_restartRequestAt = 0;
            features::OnlineLogLine(features::RestartLevel() ? "Online: restarting the mission at a player's request"
                                                             : "[fail] Online: restart at a player's request refused");
        } else if (GetTickCount() - g_restartRequestAt > 60000) {
            g_restartRequestAt = 0;
        }
    }
    const DWORD now = GetTickCount();
    static DWORD startedAt = 0;
    static bool reported = false;
    if (!g_seq) startedAt = now, reported = false;
    static DWORD lastReport = 0;
    if (now - startedAt > 5000 && now - lastReport >= 10000) {   // who runs what, every 10 s
        lastReport = now;
        reported = true;
        uint32_t maxId = 0;
        uint8_t** table = Table(maxId);
        int own = 0, puppets = 0;
        std::string squad;
        for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id) {
            const uint8_t* s = table[id];
            if (!IsSoldier(s)) continue;
            Local(s) ? ++own : ++puppets;
            if (SquadSlot(s) >= 0) {
                char one[128];
                const float* pos = reinterpret_cast<const float*>(s + 0x30);
                snprintf(one, sizeof one, " %u:P%d(slot %d%s%s, %.0f %.0f %.0f, health %.1f)", id, OwnerOf(s) + 1,
                         SquadSlot(s), Mounted(s) ? ", mounted" : "", IsPuppet(s) ? ", puppet" : "", pos[0], pos[1],
                         pos[2], *reinterpret_cast<const float*>(s + 0x54));
                squad += one;
            }
        }
        for (int p = 0; p < g_players; ++p) {
            const uint8_t* bs = *reinterpret_cast<uint8_t* const*>(kBlocks + p * kBlockSize + 0x310);
            squad += " | block " + std::to_string(p + 1) + ": " + (bs ? std::to_string(IdOf(bs)) : std::string("none"));
        }
        char line[1024];
        snprintf(line, sizeof line, "Authority: this PC runs %d soldier(s), %d follow records (sent %u, applied %u, "
                 "events %u/%u/%u, projectiles %u/%u, vehicles %u/%u, messages %u/%u, joiner actions %u, doors %u/%u, other "
                 "starts %u last %08X); squad:%s", own, puppets, g_sentRecords, g_appliedRecords, g_damageSent, g_firesSent,
                 g_firesReplayed, g_spawnsSent, g_spawnsReplayed, g_vehicleRecords, g_vehiclesApplied, g_messagesSent,
                 g_messagesDelivered, g_messagesForwarded, g_doorsSent, g_doorsApplied, g_otherEpochs, g_lastOtherEpoch,
                 squad.c_str());
        features::OnlineLogLine(line);
    }
#ifndef DS_DIST
    // dev: Dev\AuthorityDump = 1 - every 5 s the host-owned enemy nearest to player 1's soldier (soldier 0x2B10 B +
    // render instance 0x2C8 B) is appended to DesertStormFix-adump-<player>.bin: {u32 n, u32 id, soldier, instance};
    // the owner's copy vs the puppet (tools: what a puppet lacks, e.g. the aiming pose)
    static DWORD dumpAt = 0;
    static uint32_t dumps = 0;
    if (now - dumpAt >= 5000) {
        dumpAt = now;
        DWORD on = 0, size = sizeof on;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "AuthorityDump", RRF_RT_REG_DWORD, nullptr, &on,
                     &size);
        const uint8_t* me = *reinterpret_cast<uint8_t* const*>(kBlocks + 0x310);
        uint32_t maxId = 0;
        uint8_t** table = Table(maxId);
        const uint8_t* best = nullptr;
        float bestD = 1e30f;
        for (uint32_t id = 0; on && IsSoldier(me) && table && id <= maxId && id < 0xFFFF; ++id) {
            const uint8_t* e = table[id];
            if (!IsSoldier(e) || e[0x10] == 0 || SquadSlot(e) >= 0 || *reinterpret_cast<const float*>(e + 0x54) <= 0.0f)
                continue;
            float d = 0;
            for (int k = 0; k < 3; ++k) {
                const float v = reinterpret_cast<const float*>(e + 0x30)[k] - reinterpret_cast<const float*>(me + 0x30)[k];
                d += v * v;
            }
            if (d < bestD) bestD = d, best = e;
        }
        if (best) {
            char name[64];
            snprintf(name, sizeof name, "DesertStormFix-adump-%d.bin", g_local + 1);
            if (FILE* f = fopen(name, dumps ? "ab" : "wb")) {
                const uint32_t head[2] = {dumps, IdOf(best)};
                fwrite(head, 4, 2, f);
                fwrite(best, 1, 0x2B10, f);
                const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(best + 0x2C);
                uint8_t zero[0x2C8] = {};
                fwrite(inst ? inst : zero, 1, 0x2C8, f);
                fclose(f);
                ++dumps;
            }
        }
    }
#endif
#ifndef DS_DIST
    // dev: Dev\AuthorityHurt = 1 - the host makes an enemy hit player 2's soldier for 5 every 8 s and heals it by 3
    // 4 s later, through the soldier's own vtable (TakeDamage / Heal): the cross-PC damage and heal paths in a soak
    static DWORD hurtAt = 0;
    static int hurtStep = 0;
    if (g_host && g_players > 1 && now - hurtAt >= 4000) {
        DWORD on = 0, size = sizeof on;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "AuthorityHurt", RRF_RT_REG_DWORD, nullptr,
                     &on, &size);
        hurtAt = now;
        uint8_t* victim = *reinterpret_cast<uint8_t**>(kBlocks + kBlockSize + 0x310);
        // = 2: knocked down (damage = its health) and revived by a heal of 50 8 s later - a downed squad mate's revive
        static uint8_t* downed = nullptr;
        static DWORD downedAt = 0, restUntil = 0;
        if (on == 2 && downed && now - downedAt >= 8000) {
            const auto* vt = *reinterpret_cast<uint32_t* const*>(downed);
            reinterpret_cast<HealFn>(vt[14])(downed, 50.0f);
            char line[128];
            snprintf(line, sizeof line, "Authority: [dev] healed downed soldier %u (health now %.1f)", IdOf(downed),
                     *reinterpret_cast<float*>(downed + 0x54));
            features::OnlineLogLine(line);
            downed = nullptr;
            restUntil = now + 16000;   // (a while up before the next knock-down)
        } else if (on == 2 && !downed && static_cast<int>(now - restUntil) >= 0 && IsSoldier(victim) &&
                   *reinterpret_cast<float*>(victim + 0x54) > 20.0f) {
            const auto* vt = *reinterpret_cast<uint32_t* const*>(victim);
            reinterpret_cast<DamageFn>(vt[15])(victim, *reinterpret_cast<float*>(victim + 0x54), nullptr);
            downed = victim;
            downedAt = now;
            char line[128];
            snprintf(line, sizeof line, "Authority: [dev] knocked down player 2's soldier %u", IdOf(victim));
            features::OnlineLogLine(line);
        }
        if (on == 1 && IsSoldier(victim) && *reinterpret_cast<float*>(victim + 0x54) > 20.0f) {
            uint8_t* attacker = nullptr;
            uint32_t maxId = 0;
            uint8_t** table = Table(maxId);
            for (uint32_t id = 0; table && id <= maxId && !attacker; ++id)
                if (IsSoldier(table[id]) && table[id][0x10] != 0 && *reinterpret_cast<float*>(table[id] + 0x54) > 0.0f)
                    attacker = table[id];
            const auto* vt = *reinterpret_cast<uint32_t* const*>(victim);
            if (hurtStep++ % 2 == 0) reinterpret_cast<DamageFn>(vt[15])(victim, 5.0f, attacker);
            else reinterpret_cast<HealFn>(vt[14])(victim, 3.0f);
            char line[128];
            snprintf(line, sizeof line, "Authority: [dev] %s player 2's soldier %u (attacker %u)",
                     hurtStep % 2 ? "hurt" : "healed", IdOf(victim), IdOf(attacker));
            features::OnlineLogLine(line);
        }
    }
#endif
#ifndef DS_DIST
    // dev: Dev\AuthorityKillMe = 1 - this PC's own soldier dies (mission failed: the restart path in a soak);
    // Dev\AuthorityLauncher = 1 - every 12 s this PC's player fires his rifle's M203 (fire mode 0x100) once through the
    // trigger wrapper ("EA F") - the projectile replay of launchers
    static DWORD devAt = 0, launchAt = 0;
    if (now - devAt >= 1000) {
        devAt = now;
        auto dev = [](const char* name) {
            DWORD v = 0, size = sizeof v;
            RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
            return v;
        };
        uint8_t* me = *reinterpret_cast<uint8_t**>(kBlocks + g_local * kBlockSize + 0x310);
        if (IsSoldier(me) && Local(me) && *reinterpret_cast<float*>(me + 0x54) > 0.0f) {
            if (g_host && dev("AuthorityKillMe")) {   // (the registry is shared by both copies on one PC)
                const auto* vt = *reinterpret_cast<uint32_t* const*>(me);
                reinterpret_cast<DamageFn>(vt[15])(me, *reinterpret_cast<float*>(me + 0x54) + 1000.0f, nullptr);
                features::OnlineLogLine("Authority: [dev] own soldier killed");
            }
            uint8_t* weapon = *reinterpret_cast<uint8_t**>(me + 0x26B0);
            const uint8_t* def = weapon ? *reinterpret_cast<uint8_t* const*>(weapon + 0x24) : nullptr;
            if (dev("AuthorityLauncher") && now - launchAt >= 12000 && def &&
                (*reinterpret_cast<const uint32_t*>(def + 0x280) & 0x100)) {
                launchAt = now;
                const uint32_t mode = *reinterpret_cast<uint32_t*>(weapon + 0x5C);
                reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kSetFireMode)(weapon, 0x100, 0);
                Trigger(weapon, nullptr, me, 0, 0, 0);
                reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kSetFireMode)(weapon, mode, 0);
                features::OnlineLogLine("Authority: [dev] M203 fired");
            }
        }
    }
#endif
#ifndef DS_DIST
    // dev: Dev\AuthorityThrow = 1 - this PC's player throws one of his grenades every 10 s (FUN_004a1470, the game's
    // throw release): the projectile replay on the other PCs in a soak
    static DWORD throwAt = 0;
    if (now - throwAt >= 10000) {
        throwAt = now;
        DWORD on = 0, size = sizeof on;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "AuthorityThrow", RRF_RT_REG_DWORD, nullptr,
                     &on, &size);
        uint8_t* me = *reinterpret_cast<uint8_t**>(kBlocks + g_local * kBlockSize + 0x310);
        if (on && IsSoldier(me) && Local(me))
            if (uint8_t** inv = *reinterpret_cast<uint8_t***>(me + 0x26B8))
                for (int k = 0; k < 25; ++k) {
                    const uint8_t* def = inv[k] ? *reinterpret_cast<uint8_t* const*>(inv[k] + 0x24) : nullptr;
                    if (!def || *reinterpret_cast<const uint32_t*>(def + 0xC0) != 7) continue;
                    reinterpret_cast<void(__thiscall*)(void*, int)>(kSetRounds)(inv[k], 2);
                    reinterpret_cast<int(__thiscall*)(void*, void*)>(kThrow)(inv[k], me);
                    features::OnlineLogLine("Authority: [dev] grenade thrown");
                    break;
                }
    }
#endif
    if (now - g_lastRecords >= kRecordMs) {
        g_lastRecords = now;
        // each player's soldier, announced whenever it changes here (switching squad members, the game's player set-up
        // after a cutscene, a death) and the own player's every second (a lost command): the others apply it to that
        // block. Mission 1's captive Foley isn't on the squad's team list, so the game's picker FUN_0044ada0 can't give
        // him back to player 2 after the intro cutscene - only the PC where he still was could (2-PC soak 2026-10-15:
        // the joiner's own block stayed empty).
        // (the own player's block: on a change and every second; the host repeats all blocks every second)
        static DWORD lastAll = 0;
        const bool all = now - lastAll >= 1000;
        if (all) lastAll = now;
        for (int p = 0; p < g_players && p < 4; ++p) {
            const auto* bs = *reinterpret_cast<uint8_t* const*>(kBlocks + p * kBlockSize + 0x310);
            if (!IsSoldier(bs)) continue;
            const bool mine = Mine(p), changed = IdOf(bs) != g_blockSent[p];
            if (mine) g_blockSent[p] = IdOf(bs);
            if ((mine && (changed || all)) || (g_host && all))
                SendEA("EA S " + std::to_string(p) + " " + std::to_string(IdOf(bs)));
        }
        SendRecords();
        SendVehicleRecords();
    }
    WatchMounts();
    for (auto& [id, p] : g_vehicles) {
        if (!p.fresh) continue;
        uint8_t* v = ById(id);
        if (VehiclePuppet(v)) ApplyVehicle(v, p.rec);
        p.fresh = false;
    }
    for (auto& [id, p] : g_puppets) {
        if (!p.fresh) continue;
        uint8_t* s = ById(id);
        if (IsPuppet(s)) ApplyRecord(s, p);
        p.fresh = false;
    }
}
