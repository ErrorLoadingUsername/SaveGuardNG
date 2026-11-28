#pragma once

// Plain data shared between the checker (main.cpp, game thread) and the
// SKSE Menu Framework page (ui.cpp, render thread). No CommonLib types here.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace sg
{
	struct UiEntry
	{
		std::uint32_t handle{ 0 };
		std::uint32_t stackId{ 0 };
		std::string   className;
		std::string   eventName;  // ModEvent name (PapyrusModEvent::eventName_)
		std::string   state;   // "läuft" / "beendet" / "KAPUTT"
		std::string   script;  // owning script and function, if known
		int           level{ 0 };  // 0 finished, 1 running, 2 broken (for the colour)
	};

	struct UiStatus
	{
		bool                 active{ false };
		std::string          info{ "noch nicht initialisiert" };
		int                  level{ 0 };  // 0 = sicher, 1 = warten, 2 = Absturzgefahr
		std::string          overall{ "-" };
		std::string          lastCheck{ "-" };
		std::size_t          slots{ 0 };
		std::vector<UiEntry> entries;
		std::uint64_t        checks{ 0 };
		std::uint64_t        probes{ 0 };
		std::uint64_t        dangers{ 0 };
		std::string          lastDanger{ "-" };
		std::string          lastBusyScript{ "-" };
		std::string          autoSaveState{ "bereit" };   // what the cell-change autosave is doing
		std::string          lastAutoSave{ "-" };
		std::uint64_t        autoSaves{ 0 };
		std::uint64_t        autoSavesSkipped{ 0 };
		std::uint64_t        savesBlocked{ 0 };   // own saves that never reached the disk
		std::uint64_t        savesRescued{ 0 };   // saves that would have crashed - defused right before writing
		std::string          lastRescue{ "-" };
	};

	UiStatus GetStatus();
	void     RequestCheck();

	inline std::atomic<bool> g_notifyBackground{ true };
	inline std::atomic<bool> g_notifyJournal{ true };
	inline std::atomic<bool> g_logDetails{ true };
	inline std::atomic<int>  g_pollMs{ 250 };  // background check interval (100 - 2000 ms)
	inline std::atomic<bool> g_autoSave{ false };    // autosave after a loading screen, once it is safe
	inline std::atomic<int>  g_autoSaveSlots{ 3 };   // SaveGuard_Auto1 .. N, rotating
	inline std::atomic<int>  g_autoSaveTimeout{ 60 };  // give up after this many seconds
	inline std::atomic<bool> g_english{ true };       // language of the menu page and notifications

	// more autosave triggers
	inline std::atomic<bool> g_autoSleepWait{ true };   // after sleeping / waiting
	inline std::atomic<int>  g_autoInterval{ 0 };       // every N minutes of play, 0 = off
	inline std::atomic<bool> g_autoLevelUp{ false };    // after a level up
	inline std::atomic<bool> g_autoQuest{ false };      // after a journal quest is completed
	// conditions
	inline std::atomic<int>  g_autoMinGap{ 120 };       // seconds between two autosaves
	inline std::atomic<bool> g_autoSkipScenes{ true };  // not while movement is disabled (OStim etc.) + 5 s
	inline std::atomic<bool> g_autoSkipSneak{ false };
	inline std::atomic<bool> g_autoSkipMount{ false };
	inline std::atomic<bool> g_autoOnlyInterior{ false };  // autosaves only inside (interior cells)
	inline std::atomic<bool> g_autoSkipJail{ true };
	// quicksave
	inline std::atomic<bool> g_guardQuicksave{ true };  // hold F5 back while saving is unsafe
	// save files
	inline std::atomic<bool> g_regularSaves{ false };   // autosaves and held-back quicksaves as regular saves (Save<N>)

	// compatibility with other mods that block or make saves (Safer Saving SKSE)
	inline std::atomic<bool> g_respectOtherSavers{ true };  // INI: warnings only when one is installed
	inline std::atomic<bool> g_otherSaverFound{ false };    // detected at startup
	inline std::atomic<bool> g_efRegularQuicksaves{ false };  // Engine Fixes turns quicksaves into regular saves
	inline bool WarningsOnly() { return g_otherSaverFound && g_respectOtherSavers; }

	// pick the text for the current language
	// 0 = German, 1 = English, 2 = Simplified Chinese
	inline std::atomic<int> g_language{ 1 };

	inline const char* L(const char* a_de, const char* a_en, const char* a_zh = nullptr)
	{
		switch (g_language.load()) {
		case 0:
			return a_de;
		case 2:
			return a_zh ? a_zh : a_en;  // fall back to English when a string has no Chinese text yet
		default:
			return a_en;
		}
	}

	void SaveSettings();  // main.cpp, writes SaveGuard.ini

	void RegisterMenu();  // ui.cpp
}

