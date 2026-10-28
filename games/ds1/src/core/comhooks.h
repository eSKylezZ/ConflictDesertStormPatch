#pragma once
// COM vtable slots through the hooks broker (docs/sdk/reference/hooks.md "DS1 host" > "COM vtables").
//
// A vtable is shared memory per COM class: every object of the class points at the same table, so one pointer hook
// (patch::HookSlot = broker HookPointer) on `&vtable[index]` catches every object - a second device of the same class
// needs nothing new. A different class (another D3D8 implementation: d3d8.dll vs the D3D9 wrapper in src/render, a
// wrapper DLL next to the exe) has its own vtable and gets its own record here: its own `next` per slot, so each
// class's chain ends at its own original. Hook functions find the right `next` from the object (its vtable pointer
// never changes, whatever other plugins put into the slots), so other plugins can chain in front of or behind the
// feature's hook on any of these slots at their own priority.
//
//   comhooks::Table<80> g_dev;                                        // up to 80 slots, 4 classes
//   using PresentFn = HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*);
//   const comhooks::Next<comhooks::Table<80>, g_dev, 15, PresentFn> g_present;   // g_present(dev, ...) = the chain's next
//   HRESULT __stdcall Present(void* dev, ...) { ...; return g_present(dev, ...); }
//   g_dev.Hook(dev, 15, &Present);                                    // owner = the active patch::OwnerScope
#include <windows.h>

#include <cstdint>

#include "core/patch.h"

namespace comhooks {
inline void** Vtable(void* obj) { return *static_cast<void***>(obj); }

template <int Slots, int Classes = 4>
class Table {
public:
    // Hooks slot `index` of obj's class with fn at `priority` (broker pointer hook; owner = the active OwnerScope).
    // Already hooked for that class = true (idempotent: CreateDevice again, a second device, Reset). False: index out
    // of range, every class record used, or the broker refused (a `[fail]` line says why).
    bool Hook(void* obj, int index, const void* fn, int32_t priority = 0) {
        if (!obj || !fn || index < 0 || index >= Slots) return false;
        AcquireSRWLockExclusive(&lock_);
        bool ok = false;
        if (Class* c = FindOrAdd(Vtable(obj))) {
            if (c->hooked[index]) {
                ok = true;
            } else if (patch::HookSlot(reinterpret_cast<uint32_t>(&c->vtable[index]), fn, &c->next[index], 0,
                                       priority)) {
                // HookSlot also says true when the slot already held fn without a record of ours (written there by
                // someone else): no next to call - refused, calling would only recurse.
                ok = c->next[index] != nullptr;
                c->hooked[index] = ok ? 1 : 0;
                c->fn[index] = fn;
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return ok;
    }
    // Takes fn's hook off slot `index` of obj's class (the slot gets its original value back when its last hook goes).
    bool Unhook(void* obj, int index) {
        if (!obj || index < 0 || index >= Slots) return false;
        AcquireSRWLockExclusive(&lock_);
        bool ok = true;
        Class* c = Find(Vtable(obj));
        if (c && c->hooked[index]) {
            ok = patch::UnhookSlot(reinterpret_cast<uint32_t>(&c->vtable[index]), c->fn[index]);
            if (ok) c->hooked[index] = 0;   // next stays: a thread still inside the hook finishes through the chain
        }
        ReleaseSRWLockExclusive(&lock_);
        return ok;
    }
    // What fn should call for obj: the next subscriber / the class's original. Lock-free (classes are only added,
    // and published after their record is complete). Unknown class or slot not hooked = whatever the slot holds.
    void* Next(void* obj, int index) const {
        void** vt = Vtable(obj);
        const LONG n = count_;
        for (LONG i = 0; i < n; ++i)
            if (classes_[i].vtable == vt) {
                if (void* p = classes_[i].next[index]) return p;
                break;
            }
        return vt[index];
    }
    bool Hooked(void* obj, int index) const {
        void** vt = Vtable(obj);
        const LONG n = count_;
        for (LONG i = 0; i < n; ++i)
            if (classes_[i].vtable == vt) return classes_[i].hooked[index] != 0;
        return false;
    }
    // Some class has slot `index` hooked (a feature's "is my hook in?" check).
    bool AnyHooked(int index) const {
        const LONG n = count_;
        for (LONG i = 0; i < n; ++i)
            if (classes_[i].hooked[index]) return true;
        return false;
    }
    int ClassCount() const { return static_cast<int>(count_); }
    void** VtableOf(int i) const { return i >= 0 && i < count_ ? classes_[i].vtable : nullptr; }

    // Tests only (pse_tests): forgets every record (the broker / patch.cpp records are reset by their own calls).
    void ResetForTests() {
        AcquireSRWLockExclusive(&lock_);
        for (auto& c : classes_) c = Class{};
        count_ = 0;
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    struct Class {
        void** vtable = nullptr;
        void* next[Slots] = {};       // broker next variables (stable addresses: the table is static storage)
        const void* fn[Slots] = {};
        uint8_t hooked[Slots] = {};
    };
    Class* Find(void** vt) {
        for (LONG i = 0; i < count_; ++i)
            if (classes_[i].vtable == vt) return &classes_[i];
        return nullptr;
    }
    Class* FindOrAdd(void** vt) {
        if (Class* c = Find(vt)) return c;
        if (count_ >= Classes) return nullptr;
        Class& c = classes_[count_];
        c = Class{};
        c.vtable = vt;
        MemoryBarrier();
        InterlockedIncrement(&count_);
        return &c;
    }

    Class classes_[Classes] = {};
    volatile LONG count_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
};

// A callable for "the next function in the chain" of one slot, looked up per object: `next(obj, args...)`.
// `if (next)` = some class has the slot hooked. Fn = the slot's stdcall pointer type (`this` first).
template <class Tab, Tab& table, int Index, class Fn>
struct Next {
    template <class... A>
    auto operator()(void* obj, A... a) const {
        return reinterpret_cast<Fn>(table.Next(obj, Index))(obj, a...);
    }
    explicit operator bool() const { return table.AnyHooked(Index); }
};
}  // namespace comhooks
