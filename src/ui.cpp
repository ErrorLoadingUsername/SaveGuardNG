// Page in the SKSE Menu Framework (Mod Control Panel): status and debug info.

#include "PCH.h"

#include "SKSEMenuFramework.h"
#include "status.h"

namespace
{
	using namespace ImGuiMCP;

	ImVec4 LevelColor(int a_level)
	{
		switch (a_level) {
		case 0:
			return ImVec4{ 0.45f, 0.90f, 0.45f, 1.0f };
		case 1:
			return ImVec4{ 1.00f, 0.80f, 0.30f, 1.0f };
		default:
			return ImVec4{ 1.00f, 0.35f, 0.35f, 1.0f };
		}
	}

	void Toggle(const char* a_label, std::atomic<bool>& a_flag)
	{
		bool v = a_flag.load();
		if (Checkbox(a_label, &v)) {
			a_flag.store(v);
			sg::SaveSettings();
		}
	}

	void Slider(const char* a_label, std::atomic<int>& a_value, int a_min, int a_max, const char* a_format)
	{
		int v = a_value.load();
		if (SliderInt(a_label, &v, a_min, a_max, a_format)) {
			a_value.store(v);
		}
		if (IsItemDeactivatedAfterEdit()) {
			sg::SaveSettings();
		}
	}

	void __stdcall RenderStatus()
	{
		// While this page is open the game is paused, so the background check
		// sleeps. Ask the game thread for a fresh check once per second.
		static auto lastRequest = std::chrono::steady_clock::time_point{};
		const auto  now         = std::chrono::steady_clock::now();
		if (now - lastRequest > 1s) {
			lastRequest = now;
			sg::RequestCheck();
		}

		const auto s = sg::GetStatus();

		SeparatorText("Status");
		TextColored(LevelColor(s.level), "%s", s.overall.c_str());
		Text(sg::L("Letzte Pruefung: %s", "Last check: %s"), s.lastCheck.c_str());
		if (Button(sg::L("Jetzt pruefen", "Check now"))) {
			sg::RequestCheck();
		}

		SeparatorText(sg::L("Offene SKSE-Objekte", "Open SKSE objects"));
		if (s.entries.empty()) {
			TextDisabled("%s", sg::L("keine", "none"));
		} else if (BeginTable("##sg_open", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
			TableSetupColumn("Handle");
			TableSetupColumn(sg::L("Typ", "Type"));
			TableSetupColumn("Event");
			TableSetupColumn("Stack");
			TableSetupColumn(sg::L("Zustand", "State"));
			TableSetupColumn(sg::L("Skript", "Script"));
			TableHeadersRow();
			for (const auto& e : s.entries) {
				TableNextRow();
				TableSetColumnIndex(0);
				Text("%u", e.handle);
				TableSetColumnIndex(1);
				Text("%s", e.className.c_str());
				TableSetColumnIndex(2);
				Text("%s", e.eventName.empty() ? "-" : e.eventName.c_str());
				TableSetColumnIndex(3);
				Text("%u", e.stackId);
				TableSetColumnIndex(4);
				// finished = green, running = yellow, danger = orange
				const ImVec4 color = e.level == 2 ? ImVec4{ 1.00f, 0.55f, 0.15f, 1.0f } :
				                     e.level == 1 ? ImVec4{ 1.00f, 0.90f, 0.35f, 1.0f } :
				                                    LevelColor(0);
				TextColored(color, "%s", e.state.c_str());
				TableSetColumnIndex(5);
				Text("%s", e.script.empty() ? "-" : e.script.c_str());
			}
			EndTable();
		}

		if (sg::g_otherSaverFound) {
			SeparatorText(sg::L("Kompatibilitaet", "Compatibility"));
			TextColored(LevelColor(1), "%s", sg::L("Safer Saving SKSE ist installiert.", "Safer Saving SKSE is installed."));
			Toggle(sg::L("Nur warnen (Safer Saving speichert)", "Warnings only (Safer Saving does the saving)"), sg::g_respectOtherSavers);
			TextWrapped("%s", sg::WarningsOnly() ?
				sg::L("Schnellspeicher-Schutz und Autospeichern von SaveGuard sind aus - Safer Saving wuerde diese Speichervorgaenge sonst blockieren. Die Warnungen bleiben an.",
					"SaveGuard's quicksave guard and autosaves are off - Safer Saving would block those saves otherwise. Warnings stay on.") :
				sg::L("Beide speichern - Safer Saving kann die Speichervorgaenge von SaveGuard blockieren (siehe \"Blockiert\" unten).",
					"Both are saving - Safer Saving can block SaveGuard's saves (see \"Blocked\" below)."));
		}

		SeparatorText(sg::L("Einstellungen", "Settings"));
		{
			int lang = sg::g_english ? 1 : 0;
			Text("%s", sg::L("Sprache:", "Language:"));
			SameLine();
			bool changed = RadioButton("Deutsch", &lang, 0);
			SameLine();
			changed |= RadioButton("English", &lang, 1);
			if (changed) {
				sg::g_english = lang == 1;
				sg::SaveSettings();
				sg::RequestCheck();  // refresh the status texts
			}
		}
		Toggle(sg::L("Meldungen im Spiel (Hintergrundpruefung)", "In-game notifications (background check)"), sg::g_notifyBackground);
		Toggle(sg::L("Meldung beim Oeffnen des Pausenmenues", "Notification when opening the pause menu"), sg::g_notifyJournal);
		Toggle(sg::L("Details in SaveGuardNG.log schreiben", "Write details to SaveGuardNG.log"), sg::g_logDetails);
		Slider(sg::L("Pruefintervall", "Check interval"), sg::g_pollMs, 100, 2000, "%d ms");
		TextDisabled("%s", sg::L("Kuerzer = genauer, kostet trotzdem kaum etwas. Meldungen nur bei Statuswechsel.",
			"Shorter = more precise, still costs almost nothing. Notifications only when the state changes."));

		SeparatorText(sg::L("Autospeichern - wann", "Autosave - when"));
		Toggle(sg::L("Nach Ladebildschirm (Tuer, Schnellreise)", "After a loading screen (door, fast travel)"), sg::g_autoSave);
		Toggle(sg::L("Nach Schlafen / Warten", "After sleeping / waiting"), sg::g_autoSleepWait);
		Toggle(sg::L("Nach Stufenaufstieg", "After a level up"), sg::g_autoLevelUp);
		Toggle(sg::L("Nach abgeschlossener Quest", "After completing a quest"), sg::g_autoQuest);
		Slider(sg::L("Alle X Minuten Spielzeit (0 = aus)", "Every X minutes of play (0 = off)"), sg::g_autoInterval, 0, 120, "%d min");

		SeparatorText(sg::L("Autospeichern - Bedingungen", "Autosave - conditions"));
		TextDisabled("%s", sg::L("Immer: nur wenn sicher, nicht im Kampf. Sonst wird gewartet.", "Always: only when safe, not in combat. Otherwise it waits."));
		Toggle(sg::L("Nicht waehrend Szenen (OStim, Steuerung gesperrt)", "Not during scenes (OStim, controls disabled)"), sg::g_autoSkipScenes);
		Toggle(sg::L("Nicht beim Schleichen", "Not while sneaking"), sg::g_autoSkipSneak);
		Toggle(sg::L("Nicht beim Reiten", "Not while riding"), sg::g_autoSkipMount);
		Toggle(sg::L("Nicht im Gefaengnis", "Not in jail"), sg::g_autoSkipJail);
		Slider(sg::L("Mindestabstand zwischen Autosaves", "Minimum time between autosaves"), sg::g_autoMinGap, 0, 900, "%d s");
		Slider(sg::L("Anzahl Speicherplaetze", "Number of save slots"), sg::g_autoSaveSlots, 1, 10, "%d");
		Slider(sg::L("Hoechstens warten", "Wait at most"), sg::g_autoSaveTimeout, 10, 300, "%d s");
		TextDisabled("%s", sg::L("Speichert als Autosave11, 12, ... (rotierend, im Ladenmenue als AUTO).", "Saves as Autosave11, 12, ... (rotating, shown as AUTO in the load menu)."));

		SeparatorText(sg::L("Schnellspeichern", "Quicksave"));
		Toggle(sg::L("Schnellspeichern (F5) zurueckhalten, solange es nicht sicher ist", "Hold quicksave (F5) back while it is not safe"), sg::g_guardQuicksave);
		TextDisabled("%s", sg::L("Gespeichert wird dann automatisch, sobald es sicher ist.", "It then saves automatically as soon as it is safe."));
		if (sg::g_efRegularQuicksaves) {
			TextDisabled("%s", sg::L("Engine Fixes macht aus Schnellspeichern normale Spielstaende - SaveGuard ebenso.",
				"Engine Fixes turns quicksaves into regular saves - SaveGuard does the same."));
		}

		SeparatorText(sg::L("Autospeichern - Zustand", "Autosave - state"));
		Text(sg::L("Zustand: %s", "State: %s"), (s.autoSaveState == "bereit" || s.autoSaveState == "ready") ? sg::L("bereit", "ready") : s.autoSaveState.c_str());
		Text(sg::L("Letztes Speichern: %s", "Last save: %s"), s.lastAutoSave.c_str());
		Text(sg::L("Gespeichert: %llu   Uebersprungen: %llu   Blockiert: %llu", "Saved: %llu   Skipped: %llu   Blocked: %llu"), static_cast<unsigned long long>(s.autoSaves),
			static_cast<unsigned long long>(s.autoSavesSkipped), static_cast<unsigned long long>(s.savesBlocked));
		if (s.savesBlocked) {
			TextDisabled("%s", sg::L("Blockiert = gespeichert, aber keine Datei angelegt (eine andere Mod hat es verhindert).",
				"Blocked = saved, but no file was written (another mod prevented it)."));
		}
		TextDisabled("%s", sg::L("Einstellungen werden in SKSE\\SaveGuardNG.ini gespeichert.", "Settings are stored in SKSE\\SaveGuardNG.ini."));

		SeparatorText("Debug");
		TextColored(LevelColor(s.active ? 0 : 2), "Plugin: %s", s.active ? sg::L("aktiv", "active") : sg::L("inaktiv", "inactive"));
		TextWrapped("%s", s.info.c_str());
		Text(sg::L("SKSE-Objektspeicher: %zu Plaetze", "SKSE object storage: %zu slots"), s.slots);
		Text(sg::L("Pruefungen: %llu   VM-Abfragen: %llu   Absturzgefahr erkannt: %llu", "Checks: %llu   VM lookups: %llu   Crash risk detected: %llu"),
			static_cast<unsigned long long>(s.checks), static_cast<unsigned long long>(s.probes),
			static_cast<unsigned long long>(s.dangers));
		Text(sg::L("Letzte Absturzgefahr: %s", "Last crash risk: %s"), s.lastDanger.c_str());
		TextWrapped(sg::L("Zuletzt blockierendes Skript: %s", "Last blocking script: %s"), s.lastBusyScript.c_str());
		TextDisabled("%s", sg::L("Log: Dokumente\\My Games\\Skyrim Special Edition\\SKSE\\SaveGuardNG.log", "Log: Documents\\My Games\\Skyrim Special Edition\\SKSE\\SaveGuardNG.log"));
	}
}

namespace sg
{
	void RegisterMenu()
	{
		// IsInstalled() only checks a relative file path, which can fail under
		// MO2's virtual file system; asking for the loaded module is reliable.
		const bool fileFound   = SKSEMenuFramework::IsInstalled();
		const bool moduleFound = GetMenuFrameworkModule() != nullptr;
		SKSE::log::info("SKSE Menu Framework: file {}, module {}", fileFound ? "found" : "missing",
			moduleFound ? "loaded" : "not loaded");
		if (!moduleFound) {
			SKSE::log::info("SKSE Menu Framework not loaded - no menu page");
			return;
		}
		SKSEMenuFramework::SetSection("SaveGuard NG");
		SKSEMenuFramework::AddSectionItem("Status", RenderStatus);
		SKSE::log::info("menu page registered in SKSE Menu Framework");
	}
}
