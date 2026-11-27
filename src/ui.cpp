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
		Text(sg::L("Letzte Pruefung: %s", "Last check: %s", "上次检查：%s"), s.lastCheck.c_str());
		if (s.savesRescued) {
			TextColored(LevelColor(1), sg::L("Absturz beim Speichern verhindert: %llu mal (zuletzt %s)", "Save crash prevented: %llu time(s) (last %s)", "已阻止存档崩溃：%llu 次（最近一次 %s）"),
				static_cast<unsigned long long>(s.savesRescued), s.lastRescue.c_str());
		} else {
			TextDisabled("%s", sg::L("Absturz beim Speichern verhindert: noch nie in dieser Sitzung", "Save crash prevented: not yet this session", "已阻止存档崩溃：本次游戏尚未发生"));
		}
		if (Button(sg::L("Jetzt pruefen", "Check now", "立即检查"))) {
			sg::RequestCheck();
		}

		SeparatorText(sg::L("Offene SKSE-Objekte", "Open SKSE objects", "打开的 SKSE 对象"));
		if (s.entries.empty()) {
			TextDisabled("%s", sg::L("keine", "none", "无"));
		} else if (BeginTable("##sg_open", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
			TableSetupColumn("Handle");
			TableSetupColumn(sg::L("Typ", "Type", "类型"));
			TableSetupColumn("Event");
			TableSetupColumn("Stack");
			TableSetupColumn(sg::L("Zustand", "State", "状态"));
			TableSetupColumn(sg::L("Skript", "Script", "脚本"));
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
			SeparatorText(sg::L("Kompatibilitaet", "Compatibility", "兼容性"));
			TextColored(LevelColor(1), "%s", sg::L("Safer Saving SKSE ist installiert.", "Safer Saving SKSE is installed.", "检测到已安装 Safer Saving SKSE。"));
			Toggle(sg::L("Nur warnen (Safer Saving speichert)", "Warnings only (Safer Saving does the saving)", "仅警告（由 Safer Saving 负责存档）"), sg::g_respectOtherSavers);
			TextWrapped("%s", sg::WarningsOnly() ?
				sg::L("Schnellspeicher-Schutz und Autospeichern von SaveGuard sind aus - Safer Saving wuerde diese Speichervorgaenge sonst blockieren. Die Warnungen bleiben an.", "SaveGuard's quicksave guard and autosaves are off - Safer Saving would block those saves otherwise. Warnings stay on.", "SaveGuard 的快速存档保护与自动存档已关闭 —— 否则 Safer Saving 会拦下这些存档操作。警告功能保持开启。") :
				sg::L("Beide speichern - Safer Saving kann die Speichervorgaenge von SaveGuard blockieren (siehe \"Blockiert\" unten).", "Both are saving - Safer Saving can block SaveGuard's saves (see \"Blocked\" below).", "两者都在存档 —— Safer Saving 可能会拦下 SaveGuard 的存档操作（见下方「被阻止」）。"));
		}

		SeparatorText(sg::L("Einstellungen", "Settings", "设置"));
		{
			int lang = sg::g_language.load();
			Text("%s", sg::L("Sprache:", "Language:", "语言："));
			SameLine();
			bool changed = RadioButton("Deutsch", &lang, 0);
			SameLine();
			changed |= RadioButton("English", &lang, 1);
			SameLine();
			changed |= RadioButton("简体中文", &lang, 2);
			if (changed) {
				sg::g_language = lang;
				sg::g_english  = lang == 1;  // keep the old flag in sync for the ini file
				sg::SaveSettings();
				sg::RequestCheck();  // refresh the status texts
			}
		}
		Toggle(sg::L("Meldungen im Spiel (Hintergrundpruefung)", "In-game notifications (background check)", "游戏内通知（后台检查）"), sg::g_notifyBackground);
		Toggle(sg::L("Meldung beim Oeffnen des Pausenmenues", "Notification when opening the pause menu", "打开暂停菜单时提示"), sg::g_notifyJournal);
		Toggle(sg::L("Details in SaveGuardNG.log schreiben", "Write details to SaveGuardNG.log", "将详情写入 SaveGuardNG.log"), sg::g_logDetails);
		Slider(sg::L("Pruefintervall", "Check interval", "检查间隔"), sg::g_pollMs, 100, 2000, "%d ms");
		TextDisabled("%s", sg::L("Kuerzer = genauer, kostet trotzdem kaum etwas. Meldungen nur bei Statuswechsel.", "Shorter = more precise, still costs almost nothing. Notifications only when the state changes.", "越短越精确，但几乎不耗性能。仅在状态变化时提示。"));

		SeparatorText(sg::L("Autospeichern - wann", "Autosave - when", "自动存档 —— 时机"));
		Toggle(sg::L("Nach Ladebildschirm (Tuer, Schnellreise)", "After a loading screen (door, fast travel)", "读条之后（开门、快速旅行）"), sg::g_autoSave);
		Toggle(sg::L("Nach Schlafen / Warten", "After sleeping / waiting", "睡眠 / 等待之后"), sg::g_autoSleepWait);
		Toggle(sg::L("Nach Stufenaufstieg", "After a level up", "升级之后"), sg::g_autoLevelUp);
		Toggle(sg::L("Nach abgeschlossener Quest", "After completing a quest", "完成任务之后"), sg::g_autoQuest);
		Slider(sg::L("Alle X Minuten Spielzeit (0 = aus)", "Every X minutes of play (0 = off)", "每 X 分钟游戏时间（0 = 关闭）"), sg::g_autoInterval, 0, 120, "%d min");

		SeparatorText(sg::L("Autospeichern - Bedingungen", "Autosave - conditions", "自动存档 —— 条件"));
		TextDisabled("%s", sg::L("Immer: nur wenn sicher, nicht im Kampf. Sonst wird gewartet.", "Always: only when safe, not in combat. Otherwise it waits.", "始终生效：仅在安全且不在战斗时存档，否则会等待。"));
		Toggle(sg::L("Nicht waehrend Szenen (OStim, Steuerung gesperrt)", "Not during scenes (OStim, controls disabled)", "不在场景中（OStim、操作被锁定）"), sg::g_autoSkipScenes);
		Toggle(sg::L("Nicht beim Schleichen", "Not while sneaking", "不在潜行时"), sg::g_autoSkipSneak);
		Toggle(sg::L("Nicht beim Reiten", "Not while riding", "不在骑乘时"), sg::g_autoSkipMount);
		Toggle(sg::L("Nur in Innenraeumen (Haeuser, Hoehlen, Dungeons)", "Only inside (houses, caves, dungeons)", "仅室内（房屋、洞穴、地牢）"), sg::g_autoOnlyInterior);
		Toggle(sg::L("Nicht im Gefaengnis", "Not in jail", "不在监狱中"), sg::g_autoSkipJail);
		Slider(sg::L("Mindestabstand zwischen Autosaves", "Minimum time between autosaves", "两次自动存档的最小间隔"), sg::g_autoMinGap, 0, 900, "%d s");
		Slider(sg::L("Anzahl Speicherplaetze", "Number of save slots", "存档栏位数量"), sg::g_autoSaveSlots, 1, 10, "%d");
		Slider(sg::L("Hoechstens warten", "Wait at most", "最长等待"), sg::g_autoSaveTimeout, 10, 300, "%d s");
		TextDisabled("%s", sg::g_regularSaves ? sg::L("Speichert als normale Spielstaende (siehe Speicherdateien unten).", "Saves as regular saves (see save files below).", "存为普通存档（见下方「存档文件」）。") :
		                                        sg::L("Speichert als Autosave11, 12, ... (rotierend, im Ladenmenue als AUTO).", "Saves as Autosave11, 12, ... (rotating, shown as AUTO in the load menu).", "存为 Autosave11、12……（循环覆盖，在读档界面显示为 AUTO）。"));

		SeparatorText(sg::L("Schnellspeichern", "Quicksave", "快速存档"));
		Toggle(sg::L("Schnellspeichern (F5) zurueckhalten, solange es nicht sicher ist", "Hold quicksave (F5) back while it is not safe", "不安全时暂缓快速存档（F5）"), sg::g_guardQuicksave);
		TextDisabled("%s", sg::L("Gespeichert wird dann automatisch, sobald es sicher ist.", "It then saves automatically as soon as it is safe.", "一旦安全就会自动存档。"));
		if (sg::g_efRegularQuicksaves) {
			TextDisabled("%s", sg::L("Engine Fixes macht aus Schnellspeichern normale Spielstaende - SaveGuard ebenso.", "Engine Fixes turns quicksaves into regular saves - SaveGuard does the same.", "Engine Fixes 会把快速存档变成普通存档 —— SaveGuard 也是如此。"));
		}

		SeparatorText(sg::L("Speicherdateien", "Save files", "存档文件"));
		Toggle(sg::L("Normale Spielstaende statt Autosave / Quicksave", "Regular saves instead of autosave / quicksave", "用普通存档代替自动存档 / 快速存档"), sg::g_regularSaves);
		TextDisabled("%s", sg::L("Autospeichern und zurueckgehaltenes Schnellspeichern schreiben dann jedes Mal einen neuen normalen Spielstand "
								 "(Save<Nr>), wie das Speichern-Menue. Diese werden nie rotiert oder geloescht - alte Spielstaende selbst aufraeumen.",
			"Autosaves and held-back quicksaves then write a new regular save every time (Save<No.>), like the Save menu. These are "
			"never rotated or removed - clean up old saves yourself.",
			"自动存档和延后的快速存档将每次都写入一个新的普通存档（Save<编号>），与存档菜单一致。"
			"这些存档不会被循环覆盖或删除 —— 旧存档请自行清理。"));

		SeparatorText(sg::L("Autospeichern - Zustand", "Autosave - state", "自动存档 —— 状态"));
		// The state text is already localised in main.cpp. Comparing it against a fixed
		// word would silently break as soon as another language is added, so just show it.
		Text(sg::L("Zustand: %s", "State: %s", "状态：%s"), s.autoSaveState.c_str());
		Text(sg::L("Letztes Speichern: %s", "Last save: %s", "上次存档：%s"), s.lastAutoSave.c_str());
		Text(sg::L("Gespeichert: %llu   Uebersprungen: %llu   Blockiert: %llu", "Saved: %llu   Skipped: %llu   Blocked: %llu", "已存档：%llu   已跳过：%llu   被阻止：%llu"), static_cast<unsigned long long>(s.autoSaves),
			static_cast<unsigned long long>(s.autoSavesSkipped), static_cast<unsigned long long>(s.savesBlocked));
		if (s.savesBlocked) {
			TextDisabled("%s", sg::L("Blockiert = gespeichert, aber keine Datei angelegt (eine andere Mod hat es verhindert).", "Blocked = saved, but no file was written (another mod prevented it).", "「被阻止」= 触发了存档，但没有生成文件（被其他模组拦下）。"));
		}
		TextDisabled("%s", sg::L("Einstellungen werden in SKSE\\SaveGuardNG.ini gespeichert.", "Settings are stored in SKSE\\SaveGuardNG.ini.", "设置保存在 SKSE\\SaveGuardNG.ini 中。"));

		SeparatorText("Debug");
		TextColored(LevelColor(s.active ? 0 : 2), "Plugin: %s", s.active ? sg::L("aktiv", "active", "已启用") : sg::L("inaktiv", "inactive", "未启用"));
		TextWrapped("%s", s.info.c_str());
		Text(sg::L("SKSE-Objektspeicher: %zu Plaetze", "SKSE object storage: %zu slots", "SKSE 对象存储：%zu 个位置"), s.slots);
		Text(sg::L("Pruefungen: %llu   VM-Abfragen: %llu   Absturzgefahr erkannt: %llu", "Checks: %llu   VM lookups: %llu   Crash risk detected: %llu", "检查次数：%llu   虚拟机查询：%llu   检出崩溃风险：%llu"),
			static_cast<unsigned long long>(s.checks), static_cast<unsigned long long>(s.probes),
			static_cast<unsigned long long>(s.dangers));
		Text(sg::L("Letzte Absturzgefahr: %s", "Last crash risk: %s", "最近一次崩溃风险：%s"), s.lastDanger.c_str());
		TextWrapped(sg::L("Zuletzt blockierendes Skript: %s", "Last blocking script: %s", "最近阻塞的脚本：%s"), s.lastBusyScript.c_str());
		TextDisabled("%s", sg::L("Log: Dokumente\\My Games\\Skyrim Special Edition\\SKSE\\SaveGuardNG.log", "Log: Documents\\My Games\\Skyrim Special Edition\\SKSE\\SaveGuardNG.log", "日志：文档\\My Games\\Skyrim Special Edition\\SKSE\\SaveGuardNG.log"));
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

