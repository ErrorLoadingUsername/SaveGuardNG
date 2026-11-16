// SaveGuard - tells you in game whether saving is safe right now.
//
// Background
// ----------
// SKSE keeps script objects (ModEvent / UICallback handles) in its
// SKSEPersistentObjectStorage until the creating script sends or releases them.
// On save, SKSE writes this storage (co-save chunk "OBMG"). Before that,
// CleanDroppedStacks() looks up the Papyrus stack that owns each entry in the
// VM's table of running stacks and reads stack+0x18 -> +0x18. If that stack is
// stale (freed memory), this read crashes SKSE (e.g. skse64_1_6_1170.dll+5D51F).
//
// SaveGuard performs exactly the same lookup, but guarded, and reports:
//   * "Speichern sicher"        - nothing open
//   * "kurz warten"             - objects open, their scripts are still running
//   * "ABSTURZGEFAHR"           - SKSE would read a stale stack -> save crashes
//
// It checks when the pause menu opens and, in the background, once per second;
// in the background it only speaks up when the state changes.
//
// SaveGuard NG: no hard-coded SKSE addresses. The storage object is found at
// runtime through SKSE's RTTI (type name -> type descriptor -> complete object
// locator -> vtable -> the one object in SKSE's .data that uses this vtable), so
// it works with every SKSE64 build (SE 1.5.97, AE 1.6.x, GOG). The VM fields are
// identical for SE and AE (see CommonLibSSE-NG VirtualMachine.h). VR is not
// supported. If anything does not look exactly as expected, it reads nothing.

#include "PCH.h"
#include "status.h"
#include "win.h"

namespace
{
	// ---- SKSE build (found at runtime) ---------------------------------------------
	constexpr std::string_view kStorageTypeName = ".?AVSKSEPersistentObjectStorage@@";
	std::uintptr_t             g_skseBase       = 0;
	std::uintptr_t             g_storageRVA     = 0;
	std::string                g_skseName;
	std::string                g_versions;  // "Skyrim 1.6.1170, SKSE 2.2.8" for the menu
	std::uint32_t              g_skseVersion = 0;  // from SKSE's load interface

	// ---- VM fields used by SKSE's CleanDroppedStacks (skse64+5D430) ----------------
	constexpr std::uintptr_t kVMStacksLock     = 0x9318;  // BSSpinLock runningStacksLock
	constexpr std::uintptr_t kVMStacksCapacity = 0x932C;  // BSTHashMap capacity (u32)
	constexpr std::uintptr_t kVMStacksEntries  = 0x9348;  // BSTHashMap entries
	constexpr std::uintptr_t kHashEntrySize    = 0x18;    // {u32 key, Stack* value, next}

	// ---- SKSEPersistentObjectStorage layout (verified in skse64+1AEBB) ------------
	struct Entry
	{
		void*         obj;            // ISKSEObject*
		std::uint32_t owningStackId;  // Papyrus stack that created it
		std::uint32_t pad;
	};
	static_assert(sizeof(Entry) == 0x10);

	struct StorageLayout
	{
		void*        vtbl;       // 00
		std::byte    lock[40];   // 08 ICriticalSection
		Entry*       begin;      // 30 std::vector<Entry>
		Entry*       end;        // 38
		Entry*       cap;        // 40
		std::size_t* freeBegin;  // 48 std::vector<size_t> free indices
		std::size_t* freeEnd;    // 50
		std::size_t* freeCap;    // 58
	};
	static_assert(offsetof(StorageLayout, begin) == 0x30);
	static_assert(offsetof(StorageLayout, freeBegin) == 0x48);

	StorageLayout*        g_storage = nullptr;
	std::filesystem::path g_logPath;  // where SaveGuard.log is written

	// status shown on the SKSE Menu Framework page
	std::mutex   g_statusMutex;
	sg::UiStatus g_status;

	std::string NowString()
	{
		const auto t = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &t);
		char buf[16]{};
		std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
		return buf;
	}

	void SetInfo(bool a_active, std::string a_info)
	{
		std::scoped_lock l(g_statusMutex);
		g_status.active = a_active;
		g_status.info   = std::move(a_info);
	}

	enum class StackState
	{
		kGone,   // not in the VM table (or finished) -> SKSE frees the object, save is fine
		kAlive,  // script still running -> save is fine, but wait a moment
		kStale   // table points to freed memory -> SKSE crashes while saving
	};

	struct OpenObject
	{
		std::uint32_t handle{ 0 };
		std::uint32_t stackId{ 0 };
		std::string   className;
		std::string   eventName;
		std::string   script;
		StackState    state{ StackState::kGone };
	};

	// ---- guarded raw reads (no C++ objects in these functions: SEH) ----------------

	// Mirrors CleanDroppedStacks: find the stack, then read stack+0x18 -> +0x18.
	StackState ProbeStackRaw(std::uintptr_t a_vm, std::uint32_t a_id, void** a_stackOut)
	{
		*a_stackOut = nullptr;
		__try {
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(a_vm + kVMStacksCapacity);
			const auto entries  = *reinterpret_cast<const std::uintptr_t*>(a_vm + kVMStacksEntries);
			if (!entries) {
				return StackState::kGone;
			}
			for (std::uint32_t i = 0; i < capacity; ++i) {
				const auto e = entries + i * kHashEntrySize;
				if (*reinterpret_cast<const std::uintptr_t*>(e + 0x10) == 0) {
					continue;  // empty slot
				}
				if (*reinterpret_cast<const std::uint32_t*>(e) != a_id) {
					continue;
				}
				const auto stack = *reinterpret_cast<const std::uintptr_t*>(e + 8);
				if (!stack) {
					return StackState::kGone;
				}
				*a_stackOut = reinterpret_cast<void*>(stack);
				const auto p = *reinterpret_cast<const std::uintptr_t*>(stack + 0x18);
				if (!p) {
					return StackState::kGone;
				}
				const auto q = *reinterpret_cast<const std::uintptr_t*>(p + 0x18);  // the crashing read
				return q ? StackState::kAlive : StackState::kGone;
			}
			return StackState::kGone;
		} __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
			return StackState::kStale;
		}
	}

	void Append(char* a_buf, std::size_t a_len, std::size_t& a_pos, const char* a_text)
	{
		for (; a_text && *a_text && a_pos + 1 < a_len; ++a_text) {
			a_buf[a_pos++] = *a_text;
		}
		a_buf[a_pos] = '\0';
	}

	bool DescribeStackRaw(void* a_stack, char* a_buf, std::size_t a_len)
	{
		std::size_t pos = 0;
		a_buf[0]        = '\0';
		__try {
			auto stack = static_cast<RE::BSScript::Stack*>(a_stack);
			auto frame = stack->top;
			for (int depth = 0; frame && depth < 4; ++depth, frame = frame->previousFrame) {
				auto fn = frame->owningFunction.get();
				if (!fn) {
					continue;
				}
				if (pos) {
					Append(a_buf, a_len, pos, " <- ");
				}
				Append(a_buf, a_len, pos, fn->GetObjectTypeName().c_str());
				Append(a_buf, a_len, pos, ".");
				Append(a_buf, a_len, pos, fn->GetName().c_str());
			}
			return true;
		} __except (1 /* EXCEPTION_EXECUTE_HANDLER */) {
			return false;
		}
	}

	// ---- setup ------------------------------------------------------------------------
	// RTTI walk, all reads inside SKSE's own mapped sections.
	std::uintptr_t FindStorageByRTTI(std::uintptr_t a_base, std::size_t& a_candidates)
	{
		a_candidates = 0;
		std::uintptr_t rdata = 0, data = 0;
		std::size_t    rdataSize = 0, dataSize = 0;
		if (!sg::win::Section(a_base, ".rdata", rdata, rdataSize) || !sg::win::Section(a_base, ".data", data, dataSize)) {
			SKSE::log::error("SKSE sections not found");
			return 0;
		}

		// 1) type descriptor: { vftable*, spare, "name" } - the name lives in .data
		std::uintptr_t td = 0;
		{
			const auto  name = kStorageTypeName;
			const auto* p    = reinterpret_cast<const char*>(data);
			for (std::size_t i = 0x10; i + name.size() < dataSize; ++i) {
				if (p[i] == name[0] && std::memcmp(p + i, name.data(), name.size()) == 0 && p[i + name.size()] == '\0') {
					td = data + i - 0x10;
					break;
				}
			}
		}
		if (!td) {
			SKSE::log::error("RTTI type {} not found in SKSE", kStorageTypeName);
			return 0;
		}
		const auto tdRVA = static_cast<std::uint32_t>(td - a_base);

		// 2) complete object locator in .rdata: { 1, offset 0, cdOffset, pTypeDescriptor, pClassHierarchy, pSelf }
		std::uintptr_t col = 0;
		for (std::size_t i = 0; i + 24 <= rdataSize; i += 4) {
			const auto* c    = reinterpret_cast<const std::uint32_t*>(rdata + i);
			const auto  self = static_cast<std::uint32_t>(rdata + i - a_base);
			if (c[0] == 1 && c[1] == 0 && c[3] == tdRVA && c[5] == self) {
				col = rdata + i;
				break;
			}
		}
		if (!col) {
			SKSE::log::error("RTTI object locator not found");
			return 0;
		}

		// 3) vtable: the qword in .rdata pointing at the locator, vtable starts right after it
		std::uintptr_t vtbl = 0;
		for (std::size_t i = 0; i + 16 <= rdataSize; i += 8) {
			if (*reinterpret_cast<const std::uintptr_t*>(rdata + i) == col) {
				vtbl = rdata + i + 8;
				break;
			}
		}
		if (!vtbl) {
			SKSE::log::error("vtable of SKSEPersistentObjectStorage not found");
			return 0;
		}

		// 4) the static instance: an object in .data whose first qword is that vtable
		std::uintptr_t found = 0;
		for (std::size_t i = 0; i + sizeof(StorageLayout) <= dataSize; i += 8) {
			if (*reinterpret_cast<const std::uintptr_t*>(data + i) == vtbl) {
				++a_candidates;
				if (!found) {
					found = data + i;
				}
			}
		}
		SKSE::log::info("RTTI: type descriptor +{:X}, locator +{:X}, vtable +{:X}, {} instance(s)", tdRVA, col - a_base,
			vtbl - a_base, a_candidates);
		return a_candidates == 1 ? found : 0;
	}

	bool LocateStorage()
	{
		const auto runtime = REL::Module::get().version();
		g_versions         = "Skyrim " + runtime.string(".");

		std::wstring wname;
		g_skseBase = sg::win::FindSkseModule(wname);
		g_skseName.assign(wname.begin(), wname.end());  // the name is plain ASCII
		char skse[48];
		std::snprintf(skse, sizeof(skse), ", SKSE %u.%u.%u", (g_skseVersion >> 24) & 0xFF, (g_skseVersion >> 16) & 0xFF,
			(g_skseVersion >> 4) & 0xFFF);
		g_versions += skse;

		if (!g_skseBase) {
			SKSE::log::error("SKSE runtime DLL not found - SaveGuard disabled");
			SetInfo(false, sg::L("SKSE-DLL nicht gefunden - Plugin liest nichts.", "SKSE DLL not found - plugin reads nothing."));
			return false;
		}
		if (REL::Module::IsVR() || g_skseName.rfind("sksevr", 0) == 0) {
			SKSE::log::error("Skyrim VR is not supported - SaveGuard disabled");
			SetInfo(false, sg::L("Skyrim VR wird nicht unterstuetzt.", "Skyrim VR is not supported."));
			return false;
		}
		if (sg::win::CriticalSectionSize() != 40) {
			SKSE::log::error("unexpected CRITICAL_SECTION size - SaveGuard disabled");
			return false;
		}

		std::size_t candidates = 0;
		const auto  address    = FindStorageByRTTI(g_skseBase, candidates);
		if (!address) {
			SKSE::log::error("SKSEPersistentObjectStorage not found ({} candidate(s)) - SaveGuard disabled", candidates);
			SetInfo(false, std::string(sg::L("SKSE-Objektspeicher nicht gefunden - Plugin liest nichts. ", "SKSE object storage not found - plugin reads nothing. ")) + g_versions);
			return false;
		}

		auto       storage = reinterpret_cast<StorageLayout*>(address);
		const bool vecOk   = (!storage->begin && !storage->end) ||
		                   (storage->begin <= storage->end && storage->end <= storage->cap);
		const bool freeOk = (!storage->freeBegin && !storage->freeEnd) ||
		                    (storage->freeBegin <= storage->freeEnd && storage->freeEnd <= storage->freeCap);
		if (!vecOk || !freeOk) {
			SKSE::log::error("storage vectors look wrong - SaveGuard disabled");
			SetInfo(false, sg::L("SKSE-Objektspeicher sieht unerwartet aus - Plugin liest nichts.", "SKSE object storage looks unexpected - plugin reads nothing."));
			return false;
		}

		g_storage    = storage;
		g_storageRVA = address - g_skseBase;
		char rva[32];
		std::snprintf(rva, sizeof(rva), "+%llX", static_cast<unsigned long long>(g_storageRVA));
		SetInfo(true, g_versions + sg::L(" erkannt, Objektspeicher bei ", " detected, object storage at ") + g_skseName + rva +
			". Log: " + (g_logPath.empty() ? std::string(sg::L("<kein Log>", "<no log>")) : g_logPath.string()));
		SKSE::log::info("{}: SKSEPersistentObjectStorage at {}{}, {} slot(s)", g_versions, g_skseName, rva,
			static_cast<std::size_t>(storage->end - storage->begin));
		return true;
	}

	// ---- check --------------------------------------------------------------------------
	// 1) copy the open entries (cheap: SKSE's lock + a short vector walk)
	std::vector<OpenObject> ReadEntries()
	{
		std::vector<OpenObject> result;
		if (!g_storage) {
			return result;
		}

		sg::win::Lock(g_storage->lock);
		for (auto e = g_storage->begin; e && e < g_storage->end; ++e) {
			if (!e->obj) {
				continue;
			}
			OpenObject o;
			o.handle  = static_cast<std::uint32_t>(e - g_storage->begin) + 1;
			o.stackId = e->owningStackId;
			char name[64]{};
			o.className = sg::win::SafeClassName(e->obj, name, sizeof(name)) ? name : "<ungueltig>";
			// PapyrusModEvent : ISKSEObject, IFunctionArguments { BSFixedString eventName_; ... }
			if (o.className == "PapyrusModEvent") {
				char ev[96]{};
				if (sg::win::SafeStringAt(e->obj, 0x10, ev, sizeof(ev))) {
					o.eventName = ev;
				}
			}
			result.push_back(std::move(o));
		}
		sg::win::Unlock(g_storage->lock);
		return result;
	}

	// Script names, remembered while the owning stack was still running, so that
	// entries left behind later still show which script created them.
	std::unordered_map<std::uint64_t, std::string> g_scripts;
	std::uint64_t Key(const OpenObject& a_obj) { return (static_cast<std::uint64_t>(a_obj.handle) << 32) | a_obj.stackId; }

	// 2) look up the owning stacks the same way SKSE does when saving
	void ProbeOwners(std::vector<OpenObject>& result)
	{
		if (result.empty()) {
			return;
		}
		auto vm = reinterpret_cast<std::uintptr_t>(RE::BSScript::Internal::VirtualMachine::GetSingleton());
		if (!vm) {
			return;
		}
		auto&               lock = *reinterpret_cast<RE::BSSpinLock*>(vm + kVMStacksLock);
		RE::BSSpinLockGuard guard(lock);
		for (auto& o : result) {
			void* stack = nullptr;
			o.state     = ProbeStackRaw(vm, o.stackId, &stack);
			if (o.state == StackState::kAlive && stack) {
				char where[256]{};
				if (DescribeStackRaw(stack, where, sizeof(where))) {
					o.script = where;
					if (g_scripts.size() > 4096) {
						g_scripts.clear();
					}
					g_scripts[Key(o)] = o.script;
				}
			}
			if (o.script.empty()) {
				if (auto it = g_scripts.find(Key(o)); it != g_scripts.end()) {
					if (!it->second.empty()) {
						o.script = it->second + sg::L(" (beendet)", " (finished)");
					}
				} else {
					g_scripts.emplace(Key(o), std::string{});  // too late to see it, do not ask again
				}
			}
		}
	}

	std::vector<OpenObject> Snapshot()
	{
		auto result = ReadEntries();
		ProbeOwners(result);
		return result;
	}

	std::string ShortScript(const OpenObject& a_obj)
	{
		const auto dot = a_obj.script.find('.');
		return a_obj.script.empty() ? "?"s : a_obj.script.substr(0, dot);
	}

	const char* StateText(StackState a_state)
	{
		return a_state == StackState::kAlive ? sg::L("laeuft", "running") : a_state == StackState::kStale ? sg::L("gefahr", "danger") : sg::L("beendet", "finished");
	}

	// level: 0 safe, 1 wait, 2 danger
	void Publish(const std::vector<OpenObject>& a_open, int a_level, bool a_countProbe)
	{
		std::scoped_lock l(g_statusMutex);
		g_status.level     = a_level;
		g_status.overall   = a_level == 2 ? sg::L("Absturzgefahr - jetzt nicht speichern", "Crash risk - do not save now") :
		                     a_level == 1 ? sg::L("Kurz warten - ein Skript arbeitet noch", "Wait a moment - a script is still working") :
		                                    sg::L("Speichern sicher", "Safe to save");
		g_status.lastCheck = NowString();
		g_status.slots     = g_storage ? static_cast<std::size_t>(g_storage->end - g_storage->begin) : 0;
		g_status.checks++;
		if (a_countProbe) {
			g_status.probes++;
		}
		g_status.entries.clear();
		for (const auto& o : a_open) {
			g_status.entries.push_back({ o.handle, o.stackId, o.className, o.eventName, StateText(o.state), o.script,
				o.state == StackState::kStale ? 2 : o.state == StackState::kAlive ? 1 : 0 });
			if (o.state == StackState::kAlive && !o.script.empty()) {
				g_status.lastBusyScript = NowString() + "  " + o.script;
			}
		}
		if (a_level == 2) {
			g_status.lastDanger = NowString();
		}
	}

	void CheckAndNotify(std::string_view a_reason, bool a_notify)
	{
		const auto open = Snapshot();

		std::size_t alive = 0, stale = 0;
		for (const auto& o : open) {
			alive += o.state == StackState::kAlive;
			stale += o.state == StackState::kStale;
		}

		Publish(open, stale ? 2 : alive ? 1 : 0, !open.empty());

		// keep the log quiet: only write when something is open or the player asked
		// (the menu page refreshes every second while it is open - never log that)
		const bool writeLog = sg::g_logDetails && a_reason != "menu page"sv && (!open.empty() || a_notify);
		if (writeLog) {
			SKSE::log::info("[{}] {} open, {} running, {} STALE", a_reason, open.size(), alive, stale);
		}
		for (const auto& o : open) {
			if (!writeLog) {
				break;
			}
			const char* state = o.state == StackState::kAlive ? "running" :
			                    o.state == StackState::kStale ? "STALE  " :
			                                                    "gone   ";
			SKSE::log::info("  handle {:>4}  {:<20} {:<32} stack {:>8}  {}  {}", o.handle, o.className,
				o.eventName.empty() ? "-" : o.eventName, o.stackId, state, o.script);
		}

		if (!a_notify || !sg::g_notifyJournal) {
			return;
		}
		if (!g_storage) {
			RE::DebugNotification(sg::L("SaveGuard: inaktiv (andere SKSE-Version)", "SaveGuard: inactive (different SKSE version)"));
		} else if (stale > 0) {
			RE::DebugNotification(sg::L("SaveGuard: Absturzgefahr - jetzt nicht speichern", "SaveGuard: crash risk - do not save now"));
		} else if (alive > 0) {
			const auto it  = std::find_if(open.begin(), open.end(), [](auto& o) { return o.state == StackState::kAlive; });
			std::string msg = std::string(sg::L("SaveGuard: kurz warten - ", "SaveGuard: wait a moment - ")) + ShortScript(*it) + sg::L(" arbeitet noch", " is still working");
			RE::DebugNotification(msg.c_str());
		} else {
			RE::DebugNotification(sg::L("SaveGuard: Speichern sicher", "SaveGuard: safe to save"));
		}
	}

	// ---- background watch ---------------------------------------------------------------
	// Runs once per second on the main thread (queued via the SKSE task interface).
	// Only reports changes: "ABSTURZGEFAHR" when a stale stack appears, and
	// "Speichern sicher" once things are clean again after a noticeable busy phase.
	enum class Overall
	{
		kSafe,
		kWait,
		kDanger
	};

	Overall                         g_last        = Overall::kSafe;
	using SteadyTime = std::chrono::steady_clock::time_point;
	SteadyTime                                 g_unsafeSince{};    // start of the current unsafe phase
	bool                                       g_waitLogged = false;
	std::unordered_map<std::uint64_t, SteadyTime> g_firstSeen;     // when each open entry appeared

	// ---- saving: autosave triggers, conditions and the quicksave guard -----------------
	using Clock = std::chrono::steady_clock;

	enum class SaveKind
	{
		kAuto,   // one of the autosave triggers -> rotating SaveGuardAutoN slots
		kQuick   // a quicksave that was held back -> Quicksave0
	};

	struct PendingSave
	{
		bool              active{ false };
		SaveKind          kind{ SaveKind::kAuto };
		std::string       reason;
		Clock::time_point since{};
	};

	PendingSave       g_pending;
	bool              g_gameLoading = false;  // the next loading screen belongs to a game load
	bool              g_inGame      = false;  // a save was loaded / new game started (not main menu)
	Clock::time_point g_ignoreLoadingUntil{};
	Clock::time_point g_lastAutoSave{};       // for the minimum gap
	Clock::time_point g_lastScene{};          // last time movement controls were disabled
	std::int64_t      g_playMs         = 0;   // unpaused play time since the last save (timer)
	Clock::time_point g_lastTick{};
	float             g_sleepStartDays = -1.0f;

	// Sleeping / waiting in progress (TESSleepStart/TESWaitStart until the matching stop event).
	// Some plugins (e.g. NPC AI Process Position Fix) keep data in this window and abort the
	// game when it is saved, so neither autosaves nor a held-back quicksave run in it.
	std::atomic<bool> g_resting{ false };
	Clock::time_point g_restMenuClosed{};   // when the sleep/wait menu closed (fallback)
	bool              g_restWarned = false;
	std::atomic<int>  g_autoIndex{ 0 };       // last SaveGuardAutoN written

	void SetAutoState(std::string a_text)
	{
		std::scoped_lock l(g_statusMutex);
		g_status.autoSaveState = std::move(a_text);
	}

	void RequestSave(SaveKind a_kind, const char* a_reason)
	{
		const auto now = Clock::now();
		if (a_kind == SaveKind::kAuto) {
			if (g_pending.active) {
				return;  // already waiting for a save
			}
			const auto gap = std::chrono::seconds(std::clamp(sg::g_autoMinGap.load(), 0, 900));
			if (g_lastAutoSave != Clock::time_point{} && now - g_lastAutoSave < gap) {
				SKSE::log::info("[autosave] {}: ignored, last autosave was less than {} s ago", a_reason, gap.count());
				return;
			}
		}
		if (!g_pending.active || a_kind == SaveKind::kQuick) {
			g_pending.since  = now;
			g_pending.reason = a_reason;
		}
		g_pending.active = true;
		if (a_kind == SaveKind::kQuick) {
			g_pending.kind = SaveKind::kQuick;
		}
		SetAutoState(std::string(sg::L("wartet: ", "waiting: ")) + g_pending.reason);
		SKSE::log::info("[{}] {} - saving once it is safe", a_kind == SaveKind::kQuick ? "quicksave" : "autosave", a_reason);
	}

	void OnLoadingScreenClosed()
	{
		const auto now = Clock::now();
		if (g_gameLoading) {
			g_gameLoading        = false;
			g_ignoreLoadingUntil = now + 5s;
			return;
		}
		if (!g_inGame || now < g_ignoreLoadingUntil || !sg::g_autoSave) {
			return;  // loading screens before the first game load do not count
		}
		RequestSave(SaveKind::kAuto, sg::L("Zellenwechsel", "cell change"));
	}

	void OnSleepWaitMenu(bool a_opening)
	{
		auto calendar = RE::Calendar::GetSingleton();
		if (!calendar) {
			return;
		}
		if (a_opening) {
			g_sleepStartDays = calendar->GetDaysPassed();
			return;
		}
		g_restMenuClosed = Clock::now();
		const float start = g_sleepStartDays;
		g_sleepStartDays  = -1.0f;
		// only when time actually passed (at least half an hour)
		if (sg::g_autoSleepWait && start >= 0.0f && calendar->GetDaysPassed() - start > 0.5f / 24.0f) {
			RequestSave(SaveKind::kAuto, sg::L("Schlafen/Warten", "sleep/wait"));
		}
	}

	std::filesystem::path FindLogDir();  // below

	// The game only lists a save under the current character when the file name
	// follows its own scheme:
	//   <prefix>_<playerID lo>_<playerID hi>_<name as hex>_<location>_<number>_<yyyymmddhhmmss>_<level>_1
	// e.g. Save22_D2462A7C_0_536972656E65_WhiterunWorld_001201_20261112172352_6_1
	std::string BuildSaveName(const std::string& a_prefix)
	{
		auto player  = RE::PlayerCharacter::GetSingleton();
		auto manager = RE::BGSSaveLoadManager::GetSingleton();

		std::string hexName;
		for (const char* p = player ? player->GetName() : nullptr; p && *p; ++p) {
			char h[3];
			std::snprintf(h, sizeof(h), "%02X", static_cast<unsigned char>(*p));
			hexName += h;
		}

		std::string place = "SaveGuard";
		if (auto cell = player ? player->GetParentCell() : nullptr) {
			const char* edid = nullptr;
			if (cell->IsInteriorCell()) {
				edid = cell->GetFormEditorID();
			} else if (auto world = player->GetWorldspace()) {
				edid = world->GetFormEditorID();
			}
			if (edid && *edid) {
				place = edid;
			}
		}
		for (auto& c : place) {
			if (c == '_' || c == ' ') {
				c = '-';  // '_' separates the fields
			}
		}

		const auto t = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &t);
		char stamp[16];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);

		const auto id = manager ? manager->currentPlayerID : 0;
		char       buf[512];
		std::snprintf(buf, sizeof(buf), "%s_%08X_%X_%s_%s_000000_%s_%u_1", a_prefix.c_str(),
			static_cast<unsigned>(id & 0xFFFFFFFF), static_cast<unsigned>(id >> 32), hexName.c_str(), place.c_str(), stamp,
			player ? static_cast<unsigned>(player->GetLevel()) : 1u);
		return buf;
	}

	// <Documents>\My Games\<game>\ + sLocalSavePath (MO2 redirects this to the profile).
	std::filesystem::path SaveDirectory()
	{
		std::string local = "Saves\\";
		if (auto ini = RE::INISettingCollection::GetSingleton()) {
			if (auto setting = ini->GetSetting("sLocalSavePath:General"); setting && setting->GetString() &&
																			*setting->GetString()) {
				local = setting->GetString();
			}
		}
		return FindLogDir().parent_path() / local;
	}

	// Removes older files of one slot ("<prefix>_..."), never the one just written.
	void RemoveOldSlotFiles(const std::string& a_prefix, const std::string& a_keep)
	{
		const auto dir    = SaveDirectory();
		const auto prefix = a_prefix + "_";
		std::error_code ec;
		std::vector<std::filesystem::path> remove;
		for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
			const auto stem = f.path().stem().string();
			const auto ext  = f.path().extension().string();
			if ((ext == ".ess" || ext == ".skse") && stem.rfind(prefix, 0) == 0 && stem != a_keep) {
				remove.push_back(f.path());
			}
		}
		for (const auto& p : remove) {
			std::filesystem::remove(p, ec);
			SKSE::log::info("[save] removed old {}{}", p.filename().string(), ec ? " (failed)" : "");
		}
	}

	// PlayerCharacter flags at +0xBDC (kGoToJailQueued = 1 << 6) and +0xBDD
	// (kServingJailTime = 1 << 4). Same offsets in SE and AE; CommonLib hides them
	// in multi-target builds because VR differs, so they are read directly here.
	bool InJail(RE::PlayerCharacter* a_player)
	{
		if (REL::Module::IsVR()) {
			return false;
		}
		const auto base = reinterpret_cast<const std::uint8_t*>(a_player);
		return (base[0xBDD] & (1 << 4)) != 0 || (base[0xBDC] & (1 << 6)) != 0;
	}

	// Why an autosave has to wait right now (nullptr = go ahead).
	const char* AutoSaveBlocker(RE::PlayerCharacter* a_player)
	{
		if (a_player->IsInCombat()) {
			return sg::L("Kampf", "combat");
		}
		if (sg::g_autoSkipScenes && Clock::now() - g_lastScene < 5s) {
			return sg::L("Szene / Steuerung gesperrt", "scene / controls disabled");
		}
		if (sg::g_autoSkipSneak && a_player->IsSneaking()) {
			return sg::L("Schleichen", "sneaking");
		}
		if (sg::g_autoSkipMount && a_player->IsOnMount()) {
			return sg::L("Reiten", "riding");
		}
		if (sg::g_autoSkipJail && InJail(a_player)) {
			return sg::L("Gefaengnis", "jail");
		}
		return nullptr;
	}

	// Runs on the game thread from Poll(): the safety check and the save happen in
	// the same task, so no script can run in between.
	void TryPendingSave()
	{
		if (!g_pending.active) {
			return;
		}
		const bool quick   = g_pending.kind == SaveKind::kQuick;
		const auto waited  = Clock::now() - g_pending.since;
		const auto timeout = std::clamp(sg::g_autoSaveTimeout.load(), 10, 300);
		if (waited > std::chrono::seconds(timeout)) {
			{
				std::scoped_lock l(g_statusMutex);
				g_status.autoSavesSkipped++;
			}
			SetAutoState(std::string(sg::L("uebersprungen (nicht rechtzeitig sicher): ", "skipped (not safe in time): ")) + g_pending.reason);
			SKSE::log::info("[{}] {}: skipped, not possible within {} s", quick ? "quicksave" : "autosave", g_pending.reason, timeout);
			if (sg::g_notifyBackground || quick) {
				RE::DebugNotification(quick ? sg::L("SaveGuard: Schnellspeichern abgebrochen (nicht sicher)", "SaveGuard: quicksave cancelled (not safe)") :
				                              sg::L("SaveGuard: Autospeichern uebersprungen (nicht sicher)", "SaveGuard: autosave skipped (not safe)"));
			}
			g_pending = {};
			return;
		}
		if (!quick && waited < 3s) {
			return;  // let OnCellLoad / OnLoad scripts start first
		}
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || player->IsDead()) {
			SetAutoState(sg::L("wartet (Spieler tot)", "waiting (player dead)"));
			return;
		}
		if (g_resting) {
			// the stop event normally arrives before the menu closes; if it never comes,
			// do not block forever - but warn, the other plugin may still be mid-update
			if (g_restMenuClosed != Clock::time_point{} && Clock::now() - g_restMenuClosed > 10s &&
				!RE::UI::GetSingleton()->IsMenuOpen(RE::SleepWaitMenu::MENU_NAME)) {
				g_resting = false;
				SKSE::log::warn("[rest] sleep/wait never sent its stop event - saving may be unsafe for plugins that track it");
				if (!g_restWarned) {
					g_restWarned = true;
					RE::DebugNotification(sg::L("SaveGuard: Schlafen/Warten nicht sauber beendet - Spiel neu starten empfohlen",
						"SaveGuard: sleep/wait did not end cleanly - restarting the game is recommended"));
				}
			} else {
				SetAutoState(sg::L("wartet (Schlafen/Warten laeuft)", "waiting (sleep/wait in progress)"));
				return;
			}
		}
		if (!quick) {
			if (const auto why = AutoSaveBlocker(player)) {
				SetAutoState(std::string(sg::L("wartet (", "waiting (")) + why + ")");
				return;
			}
		}

		const auto  open  = Snapshot();
		std::size_t alive = 0, stale = 0;
		for (const auto& o : open) {
			alive += o.state == StackState::kAlive;
			stale += o.state == StackState::kStale;
		}
		if (stale || alive) {
			SetAutoState(stale ? sg::L("wartet (Absturzgefahr)", "waiting (crash risk)") : sg::L("wartet (ein Skript arbeitet noch)", "waiting (a script is still working)"));
			return;
		}

		auto manager = RE::BGSSaveLoadManager::GetSingleton();
		if (!manager) {
			return;
		}
		std::string prefix;
		if (quick) {
			prefix = "Quicksave0";
		} else {
			const int slots = std::clamp(sg::g_autoSaveSlots.load(), 1, 10);
			const int index = g_autoIndex.load() % slots + 1;
			g_autoIndex     = index;
			// "Autosave..." makes the load menu show AUTO; 11+ never collides with the
			// game's own Autosave1-3 (iAutoSaveCount)
			prefix = "Autosave" + std::to_string(10 + index);
		}
		const auto name = BuildSaveName(prefix);

		SKSE::log::info("[{}] {}: saving {} after {} s ({} finished object(s) left, safe)", quick ? "quicksave" : "autosave",
			g_pending.reason, name, std::chrono::duration_cast<std::chrono::seconds>(waited).count(), open.size());
		manager->Save(name.c_str());
		RemoveOldSlotFiles(prefix, name);
		if (!quick) {
			RemoveOldSlotFiles("SaveGuardAuto" + std::to_string(g_autoIndex.load()), name);  // 1.0.x name
		}

		g_playMs = 0;
		if (!quick) {
			g_lastAutoSave = Clock::now();
		}
		{
			std::scoped_lock l(g_statusMutex);
			g_status.autoSaves++;
			g_status.lastAutoSave  = NowString() + "  " + prefix + "  (" + g_pending.reason + ")";
			g_status.autoSaveState = sg::L("bereit", "ready");
		}
		g_pending = {};
		sg::SaveSettings();  // remembers the slot index
		if (sg::g_notifyBackground || quick) {
			RE::DebugNotification(quick ? sg::L("SaveGuard: schnellgespeichert", "SaveGuard: quicksaved") :
			                              sg::L("SaveGuard: automatisch gespeichert", "SaveGuard: autosaved"));
		}
	}

	// ControlMap enabled-controls flags: CommonLibSSE-NG expects them at +0x118, on
	// 1.6.1130+ they are at +0x120 (verified on 1.6.1170). Bit 0 = movement.
	bool MovementEnabled(const RE::ControlMap* a_map)
	{
		static const std::ptrdiff_t offset = REL::Module::get().version() >= REL::Version{ 1, 6, 1130, 0 } ? 0x120 : 0x118;
		return (*reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_map) + offset) & 1u) != 0;
	}

	// every unpaused check: scene tracking and the timer
	void TickSaveTriggers()
	{
		const auto now = Clock::now();
		// time since the last unpaused check, capped so pauses/menus do not count
		const auto delta = g_lastTick == Clock::time_point{} ? 0 :
		                   std::min<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastTick).count(), 2000);
		g_lastTick = now;

		if (auto controls = RE::ControlMap::GetSingleton(); controls && !MovementEnabled(controls)) {
			g_lastScene = now;
		}
		const int minutes = std::clamp(sg::g_autoInterval.load(), 0, 120);
		if (minutes > 0) {
			g_playMs += delta;
			if (g_playMs >= static_cast<std::int64_t>(minutes) * 60000) {
				g_playMs = 0;
				RequestSave(SaveKind::kAuto, sg::L("Zeitintervall", "timer"));
			}
		} else {
			g_playMs = 0;
		}
		TryPendingSave();
	}

	// ---- quicksave guard: hook QuickSaveLoadHandler::ProcessButton (vtable slot 5) ----
	using ProcessButtonFn = bool (*)(RE::MenuEventHandler*, RE::ButtonEvent*);
	ProcessButtonFn g_quickSaveOriginal = nullptr;

	bool QuickSaveProcessButton(RE::MenuEventHandler* a_this, RE::ButtonEvent* a_event)
	{
		if (sg::g_guardQuicksave && g_storage && a_event && a_event->IsDown()) {
			auto events = RE::UserEvents::GetSingleton();
			if (events && a_event->QUserEvent() == events->quicksave) {
				const auto  open  = Snapshot();
				std::size_t alive = 0, stale = 0;
				for (const auto& o : open) {
					alive += o.state == StackState::kAlive;
					stale += o.state == StackState::kStale;
				}
				if (stale || alive || g_resting) {
					RequestSave(SaveKind::kQuick, sg::L("Schnellspeichern", "quicksave"));
					RE::DebugNotification(sg::L("SaveGuard: Schnellspeichern verschoben - speichert, sobald es sicher ist",
						"SaveGuard: quicksave postponed - saving as soon as it is safe"));
					return true;  // swallow the key, we save later
				}
				SKSE::log::info("[quicksave] safe - saving right away");
				RE::DebugNotification(sg::L("SaveGuard: sicher - schnellgespeichert", "SaveGuard: safe - quicksaved"));
			}
		}
		return g_quickSaveOriginal(a_this, a_event);
	}

	void InstallQuickSaveGuard()
	{
		auto controls = RE::MenuControls::GetSingleton();
		auto handler  = controls ? controls->quickSaveLoadHandler.get() : nullptr;
		if (!handler) {
			SKSE::log::warn("QuickSaveLoadHandler not found - quicksave guard off");
			return;
		}
		auto vtbl           = *reinterpret_cast<std::uintptr_t**>(handler);
		g_quickSaveOriginal = reinterpret_cast<ProcessButtonFn>(vtbl[5]);
		REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtbl[5]), reinterpret_cast<std::uintptr_t>(&QuickSaveProcessButton));
		SKSE::log::info("quicksave guard installed");
	}

	// ---- sleep / wait window ---------------------------------------------------------------
	template <class E>
	class RestSink final : public RE::BSTEventSink<E>
	{
	public:
		static RestSink* Get()
		{
			static RestSink sink;
			return &sink;
		}
		RE::BSEventNotifyControl ProcessEvent(const E*, RE::BSTEventSource<E>*) override
		{
			constexpr bool start = std::is_same_v<E, RE::TESSleepStartEvent> || std::is_same_v<E, RE::TESWaitStartEvent>;
			g_resting = start;
			if (start) {
				g_restMenuClosed = {};
			}
			SKSE::log::info("[rest] {} {}", std::is_same_v<E, RE::TESSleepStartEvent> || std::is_same_v<E, RE::TESSleepStopEvent> ? "sleep" : "wait",
				start ? "started - no saving until it ends" : "ended");
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ---- level up / quest completed -----------------------------------------------------
	class LevelSink final : public RE::BSTEventSink<RE::LevelIncrease::Event>
	{
	public:
		static LevelSink* Get()
		{
			static LevelSink sink;
			return &sink;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::LevelIncrease::Event*, RE::BSTEventSource<RE::LevelIncrease::Event>*) override
		{
			if (sg::g_autoLevelUp) {
				if (auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([] { RequestSave(SaveKind::kAuto, sg::L("Stufenaufstieg", "level up")); });
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class QuestSink final : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
	{
	public:
		static QuestSink* Get()
		{
			static QuestSink sink;
			return &sink;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent* a_event, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
		{
			if (!sg::g_autoQuest || !a_event || a_event->started) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto id = a_event->formID;
			if (auto tasks = SKSE::GetTaskInterface()) {
				tasks->AddTask([id] {
					auto quest = RE::TESForm::LookupByID<RE::TESQuest>(id);
					if (!quest || !quest->IsCompleted()) {
						return;
					}
					// only real quests from the journal, not background or misc ones
					const auto type = quest->GetType();
					if (type == RE::QUEST_DATA::Type::kNone || type == RE::QUEST_DATA::Type::kMiscellaneous) {
						return;
					}
					RequestSave(SaveKind::kAuto, sg::L("Quest abgeschlossen", "quest completed"));
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void Poll()
	{
		if (!g_storage || !RE::PlayerCharacter::GetSingleton()) {
			return;
		}
		if (auto ui = RE::UI::GetSingleton(); !ui || ui->GameIsPaused()) {
			return;  // menus are handled by the journal check, Papyrus is paused anyway
		}

		// Ignore everything that a script opens and sends within one second (that
		// is the normal case). Only entries that exist for at least a second count
		// as open - independent of how often we check.
		const auto tick    = std::chrono::steady_clock::now();
		auto       entries = ReadEntries();
		std::vector<OpenObject> open;
		std::unordered_map<std::uint64_t, SteadyTime> current;
		for (auto& e : entries) {
			const auto key   = (static_cast<std::uint64_t>(e.handle) << 32) | e.stackId;
			const auto it    = g_firstSeen.find(key);
			const auto first = it != g_firstSeen.end() ? it->second : tick;
			current.emplace(key, first);
			if (tick - first >= 1s) {
				open.push_back(e);
			}
		}
		g_firstSeen = std::move(current);
		// Look at entries we have not seen yet once, while their script still runs,
		// only to remember the script name (cheap: only when something is open).
		std::vector<OpenObject> fresh;
		for (auto& e : entries) {
			if (!g_scripts.contains(Key(e))) {
				fresh.push_back(e);
			}
		}
		ProbeOwners(fresh);
		ProbeOwners(open);

		bool alive = false, stale = false;
		for (const auto& o : open) {
			alive |= o.state == StackState::kAlive;
			stale |= o.state == StackState::kStale;
		}
		const auto now = stale ? Overall::kDanger : alive ? Overall::kWait : Overall::kSafe;
		Publish(open, static_cast<int>(now), !open.empty());

		if (now == Overall::kDanger && g_last != Overall::kDanger) {
			{
				std::scoped_lock l(g_statusMutex);
				g_status.dangers++;
			}
			if (sg::g_notifyBackground) {
				RE::DebugNotification(sg::L("SaveGuard: Absturzgefahr - jetzt nicht speichern", "SaveGuard: crash risk - do not save now"));
			}
			CheckAndNotify("watch: danger", false);  // writes the details to the log
		}
		if (now != Overall::kSafe && g_last == Overall::kSafe) {
			g_unsafeSince = tick;
			g_waitLogged  = false;
		}
		const auto unsafeFor = now != Overall::kSafe ? tick - g_unsafeSince : std::chrono::steady_clock::duration{};
		if (now == Overall::kWait && !g_waitLogged && unsafeFor >= 1s) {
			g_waitLogged = true;
			CheckAndNotify("watch: open for 2+ s", false);
		}
		// "safe" only after a real unsafe phase (danger, or waiting for a second or more)
		if (now == Overall::kSafe && g_last != Overall::kSafe &&
			(g_last == Overall::kDanger || tick - g_unsafeSince >= 1s) && sg::g_notifyBackground) {
			RE::DebugNotification(sg::L("SaveGuard: Speichern sicher", "SaveGuard: safe to save"));
		}
		g_last = now;

		TickSaveTriggers();
	}

	void StartWatch()
	{
		std::thread([] {
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(sg::g_pollMs.load(), 100, 2000)));
				if (auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask(Poll);
				}
			}
		}).detach();
	}

	// ---- events -------------------------------------------------------------------------
	class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* Get()
		{
			static MenuSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
				CheckAndNotify("journal menu", true);
			}
			if (a_event && !a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				if (auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask(OnLoadingScreenClosed);
				}
			}
			if (a_event && a_event->menuName == RE::SleepWaitMenu::MENU_NAME) {
				const bool opening = a_event->opening;
				if (auto tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([opening] { OnSleepWaitMenu(opening); });
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// Same folder SKSE uses for skse64.log: <Documents>\My Games\<game>\SKSE.
	// The Windows Documents folder comes first (it follows a moved/redirected
	// Documents folder; CommonLib's log_directory() did not on one test system),
	// then CommonLib, then the DLL folder.
	std::filesystem::path FindLogDir()
	{
		std::error_code ec;
		if (const auto docs = sg::win::DocumentsDir(); !docs.empty()) {
			for (const auto* game : { "Skyrim Special Edition", "Skyrim Special Edition GOG" }) {
				auto dir = std::filesystem::path(docs) / "My Games" / game / "SKSE";
				if (std::filesystem::is_directory(dir, ec)) {
					return dir;
				}
			}
		}
		if (auto dir = SKSE::log::log_directory(); dir && std::filesystem::is_directory(*dir, ec)) {
			return *dir;
		}
		return std::filesystem::current_path(ec) / "Data" / "SKSE" / "Plugins";
	}

	void SetupLog()
	{
		g_logPath = FindLogDir() / "SaveGuardNG.log";
		try {
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(g_logPath.string(), true);
			auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
			log->set_level(spdlog::level::info);
			log->flush_on(spdlog::level::info);
			spdlog::set_default_logger(std::move(log));
			spdlog::set_pattern("[%H:%M:%S.%e] %v");
		} catch (...) {
			g_logPath.clear();
		}
	}

	// ---- settings: <Documents>\My Games\Skyrim Special Edition\SKSE\SaveGuard.ini ------
	std::filesystem::path g_iniPath;
	std::mutex            g_iniMutex;

	void LoadSettings()
	{
		std::ifstream file(g_iniPath);
		std::string   line;
		while (std::getline(file, line)) {
			const auto eq = line.find('=');
			if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) {
				continue;
			}
			const auto key   = line.substr(0, eq);
			const int  value = std::atoi(line.c_str() + eq + 1);
			if (key == "notifyBackground") {
				sg::g_notifyBackground = value != 0;
			} else if (key == "notifyJournal") {
				sg::g_notifyJournal = value != 0;
			} else if (key == "logDetails") {
				sg::g_logDetails = value != 0;
			} else if (key == "autoSaveOnCellChange") {
				sg::g_autoSave = value != 0;
			} else if (key == "autoSaveSlots") {
				sg::g_autoSaveSlots = std::clamp(value, 1, 10);
			} else if (key == "autoSaveTimeout") {
				sg::g_autoSaveTimeout = std::clamp(value, 10, 300);
			} else if (key == "pollIntervalMs") {
				sg::g_pollMs = std::clamp(value, 100, 2000);
			} else if (key == "autoSaveSleepWait") {
				sg::g_autoSleepWait = value != 0;
			} else if (key == "autoSaveIntervalMinutes") {
				sg::g_autoInterval = std::clamp(value, 0, 120);
			} else if (key == "autoSaveLevelUp") {
				sg::g_autoLevelUp = value != 0;
			} else if (key == "autoSaveQuestCompleted") {
				sg::g_autoQuest = value != 0;
			} else if (key == "autoSaveMinGapSeconds") {
				sg::g_autoMinGap = std::clamp(value, 0, 900);
			} else if (key == "skipDuringScenes") {
				sg::g_autoSkipScenes = value != 0;
			} else if (key == "skipWhileSneaking") {
				sg::g_autoSkipSneak = value != 0;
			} else if (key == "skipWhileRiding") {
				sg::g_autoSkipMount = value != 0;
			} else if (key == "skipInJail") {
				sg::g_autoSkipJail = value != 0;
			} else if (key == "guardQuicksave") {
				sg::g_guardQuicksave = value != 0;
			} else if (key == "english") {
				sg::g_english = value != 0;
			} else if (key == "autoSaveLastIndex") {
				g_autoIndex = std::clamp(value, 0, 10);
			}
		}
	}
}

namespace sg
{
	void SaveSettings()
	{
		std::scoped_lock l(g_iniMutex);
		if (g_iniPath.empty()) {
			return;
		}
		std::ofstream file(g_iniPath, std::ios::trunc);
		file << "; SaveGuard NG settings - 1 = on, 0 = off\n"
			 << "; Can also be changed in game: SKSE Menu Framework -> SaveGuard NG -> Status (written back here).\n"
			 << "; Changes made in this file take effect at the next game start.\n"
			 << "\n[General]\n"
			 << "; language of the menu page and notifications: 0 = German, 1 = English\n"
			 << "english=" << int(g_english.load()) << "\n"
			 << "; in-game notification when the state changes (crash risk / safe to save)\n"
			 << "notifyBackground=" << int(g_notifyBackground.load()) << "\n"
			 << "; notification with the current state whenever the pause menu is opened\n"
			 << "notifyJournal=" << int(g_notifyJournal.load()) << "\n"
			 << "; write the open objects and their scripts to SaveGuardNG.log\n"
			 << "logDetails=" << int(g_logDetails.load()) << "\n"
			 << "; how often the background check runs (ms, 100-2000)\n"
			 << "pollIntervalMs=" << g_pollMs.load() << "\n"
			 << "\n[Autosave]\n"
			 << "; autosave after a loading screen (door, fast travel), once it is safe\n"
			 << "autoSaveOnCellChange=" << int(g_autoSave.load()) << "\n"
			 << "; autosave after sleeping or waiting (at least 30 in-game minutes)\n"
			 << "autoSaveSleepWait=" << int(g_autoSleepWait.load()) << "\n"
			 << "; autosave after a level up\n"
			 << "autoSaveLevelUp=" << int(g_autoLevelUp.load()) << "\n"
			 << "; autosave after completing a journal quest (not background / misc quests)\n"
			 << "autoSaveQuestCompleted=" << int(g_autoQuest.load()) << "\n"
			 << "; autosave every N minutes of play time, 0 = off (0-120)\n"
			 << "autoSaveIntervalMinutes=" << g_autoInterval.load() << "\n"
			 << "; minimum time between two autosaves (seconds, 0-900)\n"
			 << "autoSaveMinGapSeconds=" << g_autoMinGap.load() << "\n"
			 << "; number of rotating autosave slots, saved as Autosave11, Autosave12 ... (1-10)\n"
			 << "autoSaveSlots=" << g_autoSaveSlots.load() << "\n"
			 << "; give up an autosave if it is not safe within this time (seconds, 10-300)\n"
			 << "autoSaveTimeout=" << g_autoSaveTimeout.load() << "\n"
			 << "\n[Conditions]\n"
			 << "; Autosaves always wait until saving is safe and the player is not in combat.\n"
			 << "; no autosave while movement controls are disabled (OStim and other scenes) + 5 s\n"
			 << "skipDuringScenes=" << int(g_autoSkipScenes.load()) << "\n"
			 << "; no autosave while sneaking\n"
			 << "skipWhileSneaking=" << int(g_autoSkipSneak.load()) << "\n"
			 << "; no autosave while riding\n"
			 << "skipWhileRiding=" << int(g_autoSkipMount.load()) << "\n"
			 << "; no autosave while in jail\n"
			 << "skipInJail=" << int(g_autoSkipJail.load()) << "\n"
			 << "\n[Quicksave]\n"
			 << "; hold quicksave (F5) back while saving is not safe and save as soon as it is\n"
			 << "guardQuicksave=" << int(g_guardQuicksave.load()) << "\n"
			 << "\n[Internal]\n"
			 << "; last autosave slot written - do not change\n"
			 << "autoSaveLastIndex=" << g_autoIndex.load() << "\n";
	}

	UiStatus GetStatus()
	{
		std::scoped_lock l(g_statusMutex);
		return g_status;
	}

	void RequestCheck()
	{
		if (auto tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([] { CheckAndNotify("menu page", false); });
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	g_skseVersion = a_skse->SKSEVersion();
	SetupLog();
	SKSE::log::info("SaveGuard NG 1.1.6.1 loading");
	g_iniPath = FindLogDir() / "SaveGuardNG.ini";
	LoadSettings();
	sg::SaveSettings();  // rewrite with all keys and comments (keeps the loaded values)
	SetAutoState(sg::L("bereit", "ready"));
	SKSE::log::info("settings {}: autosave on cell change {}, {} slot(s), timeout {} s", g_iniPath.string(),
		sg::g_autoSave ? "on" : "off", sg::g_autoSaveSlots.load(), sg::g_autoSaveTimeout.load());

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			sg::RegisterMenu();
			if (LocateStorage()) {
				RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::Get());
				if (auto levels = RE::LevelIncrease::GetEventSource()) {
					levels->AddEventSink(LevelSink::Get());
				}
				if (auto scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
					scripts->AddEventSink<RE::TESQuestStartStopEvent>(QuestSink::Get());
					scripts->AddEventSink<RE::TESSleepStartEvent>(RestSink<RE::TESSleepStartEvent>::Get());
					scripts->AddEventSink<RE::TESSleepStopEvent>(RestSink<RE::TESSleepStopEvent>::Get());
					scripts->AddEventSink<RE::TESWaitStartEvent>(RestSink<RE::TESWaitStartEvent>::Get());
					scripts->AddEventSink<RE::TESWaitStopEvent>(RestSink<RE::TESWaitStopEvent>::Get());
				}
				InstallQuickSaveGuard();
				StartWatch();
				SKSE::log::info("watching the journal menu and checking every {} ms", std::clamp(sg::g_pollMs.load(), 100, 2000));
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			g_inGame      = true;
			g_gameLoading = true;
			g_pending     = {};
			g_playMs   = 0;
			g_lastTick = {};
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			g_inGame             = true;
			g_ignoreLoadingUntil = Clock::now() + 5s;
			CheckAndNotify("after load", false);
			break;
		default:
			break;
		}
	});
	return true;
}
