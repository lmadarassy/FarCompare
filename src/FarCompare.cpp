#define NOMINMAX 1

#include <windows.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>

#include "plugin.hpp"
#include "farcolor.hpp"
#include "FarCompareLng.hpp"
#include "version.hpp"

#include "guid.hpp"
#include <initguid.h>
#include "guid.hpp"

// ComparePlus diff engine (standalone, no Notepad++ dependency)
#include "diff.h" // from comparePlus engine

// Override SDK's MAKE_OPAQUE which uses |= (needs l-value)
#undef MAKE_OPAQUE
#define MAKE_OPAQUE(x) ((x) | 0xFF000000U)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static PluginStartupInfo PsInfo;
static FarStandardFunctions FSF;

static const wchar_t *GetMsg(int MsgId)
{
	return PsInfo.GetMsg(&MainGuid, MsgId);
}

// ---------------------------------------------------------------------------
// File reading
// ---------------------------------------------------------------------------
static std::vector<std::wstring> ReadFileLines(const wchar_t* path)
{
	std::vector<std::wstring> lines;
	FILE* f = _wfopen(path, L"rb");
	if (!f) return lines;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	std::vector<char> buf(sz + 1);
	fread(buf.data(), 1, sz, f);
	buf[sz] = 0;
	fclose(f);

	char* p = buf.data();
	if (sz >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;

	std::string current;
	auto flushLine = [&]() {
		int needed = MultiByteToWideChar(CP_UTF8, 0, current.c_str(), (int)current.size(), NULL, 0);
		std::wstring wline(needed, 0);
		MultiByteToWideChar(CP_UTF8, 0, current.c_str(), (int)current.size(), &wline[0], needed);
		lines.push_back(std::move(wline));
		current.clear();
	};

	for (; *p; ++p) {
		if (*p == '\r') {
			if (*(p+1) == '\n') ++p;
			flushLine();
		} else if (*p == '\n') {
			flushLine();
		} else {
			current += *p;
		}
	}
	if (!current.empty()) flushLine();

	return lines;
}

// ---------------------------------------------------------------------------
// Diff types for display
// ---------------------------------------------------------------------------
enum class LineType { MATCH, DEL, INS, CHANGED };

struct AlignedLine {
	std::wstring leftText;
	std::wstring rightText;
	LineType type;
	// Character-level diff ranges for CHANGED lines (positions of differences)
	std::vector<std::pair<int,int>> leftDiffRanges;  // [start,end) pairs
	std::vector<std::pair<int,int>> rightDiffRanges;
};

// Simple character-level diff: find differing ranges between two strings
static void ComputeCharDiff(const std::wstring& a, const std::wstring& b,
	std::vector<std::pair<int,int>>& aDiffs, std::vector<std::pair<int,int>>& bDiffs)
{
	// Use word-level granularity: split into tokens, diff tokens, map back to char positions
	// For simplicity, use character-level Myers diff on the two strings
	std::vector<wchar_t> va(a.begin(), a.end());
	std::vector<wchar_t> vb(b.begin(), b.end());

	DiffCalc<wchar_t> dc(va, vb);
	auto result = dc(false, false, false);

	int posA = 0, posB = 0;
	for (size_t i = 0; i < result.size(); ++i) {
		auto& d = result[i];
		if (d.type == diff_type::DIFF_MATCH) {
			posA += (int)d.len;
			posB += (int)d.len;
		} else if (d.type == diff_type::DIFF_IN_1) {
			aDiffs.push_back({posA, posA + (int)d.len});
			posA += (int)d.len;
		} else if (d.type == diff_type::DIFF_IN_2) {
			bDiffs.push_back({posB, posB + (int)d.len});
			posB += (int)d.len;
		}
	}
}

// ---------------------------------------------------------------------------
// Line wrapper for diff - compares by normalized content (ignoring timestamps)
// ---------------------------------------------------------------------------
static uint64_t HashString(const std::wstring& s)
{
	uint64_t h = 0x84222325ULL;
	for (wchar_t c : s)
		h = h * 31 + c;
	return h;
}

// Normalize a line: strip leading [timestamp] pattern like [HH:MM:SS.mmm]
static std::wstring NormalizeLine(const std::wstring& s)
{
	if (s.size() > 2 && s[0] == L'[') {
		// Find closing bracket
		auto pos = s.find(L']');
		if (pos != std::wstring::npos && pos < 30) {
			// Check if content between brackets looks like a timestamp (contains digits and colons/dots)
			bool looksLikeTimestamp = false;
			for (size_t i = 1; i < pos; ++i) {
				if (s[i] >= L'0' && s[i] <= L'9') { looksLikeTimestamp = true; break; }
			}
			if (looksLikeTimestamp) {
				// Skip past the bracket and any trailing space
				size_t start = pos + 1;
				while (start < s.size() && s[start] == L' ') ++start;
				return s.substr(start);
			}
		}
	}
	return s;
}

struct Line {
	int index;          // original line index
	uint64_t hash;      // hash of normalized content
	const std::wstring* original; // pointer to original text

	bool operator==(const Line& rhs) const { return hash == rhs.hash; }
	bool operator!=(const Line& rhs) const { return hash != rhs.hash; }
};

static std::vector<Line> MakeLines(const std::vector<std::wstring>& strings)
{
	std::vector<Line> lines;
	lines.reserve(strings.size());
	for (int i = 0; i < (int)strings.size(); ++i) {
		Line l;
		l.index = i;
		l.hash = HashString(NormalizeLine(strings[i]));
		l.original = &strings[i];
		lines.push_back(l);
	}
	return lines;
}

// ---------------------------------------------------------------------------
// Run ComparePlus diff engine and build aligned lines
// ---------------------------------------------------------------------------
static std::vector<AlignedLine> RunDiffAndAlign(
	const std::vector<std::wstring>& A,
	const std::vector<std::wstring>& B)
{
	auto linesA = MakeLines(A);
	auto linesB = MakeLines(B);

	DiffCalc<Line> dc(linesA, linesB);
	auto result = dc(true, true, true);

	std::vector<AlignedLine> aligned;

	// Track position in B separately since DIFF_MATCH.off indexes into A
	intptr_t posB = 0;

	for (size_t i = 0; i < result.size(); ++i) {
		auto& d = result[i];

		if (d.type == diff_type::DIFF_MATCH) {
			for (intptr_t j = 0; j < d.len; ++j) {
				intptr_t idxA = d.off + j;
				intptr_t idxB = posB + j;
				if (idxA < (intptr_t)linesA.size() && idxB < (intptr_t)linesB.size()) {
					if (*linesA[idxA].original == *linesB[idxB].original) {
						aligned.push_back({*linesA[idxA].original, *linesB[idxB].original, LineType::MATCH});
					} else {
						AlignedLine al;
						al.leftText = *linesA[idxA].original;
						al.rightText = *linesB[idxB].original;
						al.type = LineType::CHANGED;
						ComputeCharDiff(al.leftText, al.rightText, al.leftDiffRanges, al.rightDiffRanges);
						aligned.push_back(std::move(al));
					}
				}
			}
			posB += d.len;
		}
		else if (d.type == diff_type::DIFF_IN_1) {
			if (i + 1 < result.size() && result[i+1].type == diff_type::DIFF_IN_2) {
				auto& d2 = result[i+1];
				intptr_t common = std::min(d.len, d2.len);
				for (intptr_t j = 0; j < common; ++j) {
					AlignedLine al;
					al.leftText = *linesA[d.off + j].original;
					al.rightText = *linesB[d2.off + j].original;
					al.type = LineType::CHANGED;
					ComputeCharDiff(al.leftText, al.rightText, al.leftDiffRanges, al.rightDiffRanges);
					aligned.push_back(std::move(al));
				}
				for (intptr_t j = common; j < d.len; ++j) {
					aligned.push_back({*linesA[d.off + j].original, L"", LineType::DEL});
				}
				for (intptr_t j = common; j < d2.len; ++j) {
					aligned.push_back({L"", *linesB[d2.off + j].original, LineType::INS});
				}
				posB = d2.off + d2.len;
				++i;
			} else {
				for (intptr_t j = 0; j < d.len; ++j) {
					aligned.push_back({*linesA[d.off + j].original, L"", LineType::DEL});
				}
				// posB unchanged - deletions don't advance B
			}
		}
		else if (d.type == diff_type::DIFF_IN_2) {
			for (intptr_t j = 0; j < d.len; ++j) {
				aligned.push_back({L"", *linesB[d.off + j].original, LineType::INS});
			}
			posB = d.off + d.len;
		}
	}

	return aligned;
}

// ---------------------------------------------------------------------------
// Compare session state
// ---------------------------------------------------------------------------
struct CompareSession {
	bool active = false;
	intptr_t editorId = -1;
	std::wstring tempFile;
	std::vector<int> diffLineIndices;
	int halfWidth = 40;
};

static CompareSession g_session;
static std::vector<AlignedLine> g_alignedLines;

// ---------------------------------------------------------------------------
// Build side-by-side temp file
// ---------------------------------------------------------------------------
static std::wstring PadOrTrunc(const std::wstring& s, int width)
{
	if ((int)s.size() >= width)
		return s.substr(0, width);
	return s + std::wstring(width - (int)s.size(), L' ');
}

static std::wstring WriteTempFile(const std::vector<AlignedLine>& lines, int halfWidth)
{
	wchar_t tmpPath[MAX_PATH];
	GetTempPathW(MAX_PATH, tmpPath);
	wchar_t tmpFile[MAX_PATH];
	GetTempFileNameW(tmpPath, L"fcmp", 0, tmpFile);
	std::wstring path = tmpFile;
	path += L".cmp";
	DeleteFileW(tmpFile);

	FILE* f = _wfopen(path.c_str(), L"wb");
	if (!f) return L"";

	fwrite("\xEF\xBB\xBF", 1, 3, f);

	int hw = halfWidth - 1;
	for (size_t i = 0; i < lines.size(); ++i) {
		std::wstring merged = PadOrTrunc(lines[i].leftText, hw) + L"\x2502" + PadOrTrunc(lines[i].rightText, hw);
		int needed = WideCharToMultiByte(CP_UTF8, 0, merged.c_str(), (int)merged.size(), NULL, 0, NULL, NULL);
		std::string utf8(needed, 0);
		WideCharToMultiByte(CP_UTF8, 0, merged.c_str(), (int)merged.size(), &utf8[0], needed, NULL, NULL);
		fwrite(utf8.c_str(), 1, utf8.size(), f);
		if (i + 1 < lines.size())
			fwrite("\r\n", 1, 2, f);
	}

	fclose(f);
	return path;
}

// ---------------------------------------------------------------------------
// Editor colorization
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
void WINAPI GetGlobalInfoW(GlobalInfo *GInfo)
{
	GInfo->StructSize = sizeof(GlobalInfo);
	GInfo->MinFarVersion = FARMANAGERVERSION;
	GInfo->Version = PLUGIN_VERSION;
	GInfo->Guid = MainGuid;
	GInfo->Title = PLUGIN_NAME;
	GInfo->Description = PLUGIN_DESC;
	GInfo->Author = PLUGIN_AUTHOR;
}

void WINAPI SetStartupInfoW(const PluginStartupInfo *Info)
{
	PsInfo = *Info;
	FSF = *PsInfo.FSF;
	PsInfo.FSF = &FSF;
}

void WINAPI GetPluginInfoW(PluginInfo *Info)
{
	Info->StructSize = sizeof(*Info);
	Info->Flags = PF_EDITOR | PF_PRELOAD;
	static const wchar_t *PluginMenuStrings[1];
	PluginMenuStrings[0] = GetMsg(MTitle);
	Info->PluginMenu.Guids = &MenuGuid;
	Info->PluginMenu.Strings = PluginMenuStrings;
	Info->PluginMenu.Count = ARRAYSIZE(PluginMenuStrings);
}

static void CleanupSession()
{
	if (!g_session.tempFile.empty()) {
		DeleteFileW(g_session.tempFile.c_str());
		g_session.tempFile.clear();
	}
	g_session.active = false;
	g_session.editorId = -1;
}

static void ShowMessage(const wchar_t* title, const wchar_t* msg)
{
	const wchar_t* items[] = { title, msg, GetMsg(MOk) };
	PsInfo.Message(&MainGuid, nullptr, FMSG_WARNING, nullptr, items, ARRAYSIZE(items), 1);
}

HANDLE WINAPI OpenW(const OpenInfo *Info)
{
	PanelInfo pi = { sizeof(PanelInfo) };
	PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi);

	std::wstring file1, file2;

	size_t dirSize = PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
	auto* dir = (FarPanelDirectory*)new char[dirSize];
	dir->StructSize = sizeof(FarPanelDirectory);
	PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, (int)dirSize, dir);
	std::wstring panelDir = dir->Name;
	delete[] (char*)dir;
	if (!panelDir.empty() && panelDir.back() != L'\\') panelDir += L'\\';

	if (pi.SelectedItemsNumber == 2) {
		for (int idx = 0; idx < 2; ++idx) {
			size_t sz = PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, idx, nullptr);
			auto* item = (PluginPanelItem*)new char[sz];
			FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), sz, item };
			PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, idx, &gpi);
			std::wstring name = item->FileName;
			delete[] (char*)item;
			if (idx == 0) file1 = panelDir + name;
			else file2 = panelDir + name;
		}
	} else {
		{
			size_t sz = PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, nullptr);
			auto* item = (PluginPanelItem*)new char[sz];
			FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), sz, item };
			PsInfo.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &gpi);
			file1 = panelDir + item->FileName;
			delete[] (char*)item;
		}
		{
			size_t dirSize2 = PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
			auto* dir2 = (FarPanelDirectory*)new char[dirSize2];
			dir2->StructSize = sizeof(FarPanelDirectory);
			PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, (int)dirSize2, dir2);
			std::wstring passiveDir = dir2->Name;
			delete[] (char*)dir2;
			if (!passiveDir.empty() && passiveDir.back() != L'\\') passiveDir += L'\\';

			size_t sz = PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETCURRENTPANELITEM, 0, nullptr);
			auto* item = (PluginPanelItem*)new char[sz];
			FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), sz, item };
			PsInfo.PanelControl(PANEL_PASSIVE, FCTL_GETCURRENTPANELITEM, 0, &gpi);
			file2 = passiveDir + item->FileName;
			delete[] (char*)item;
		}
	}

	auto linesA = ReadFileLines(file1.c_str());
	auto linesB = ReadFileLines(file2.c_str());

	if (linesA.empty() && linesB.empty()) {
		ShowMessage(GetMsg(MTitle), GetMsg(MNeedTwoFiles));
		return nullptr;
	}

	// Run ComparePlus diff engine
	g_alignedLines = RunDiffAndAlign(linesA, linesB);

	// Check if all match
	bool allMatch = true;
	for (auto& al : g_alignedLines) {
		if (al.type != LineType::MATCH) { allMatch = false; break; }
	}
	if (allMatch) {
		ShowMessage(GetMsg(MTitle), GetMsg(MFilesMatch));
		g_alignedLines.clear();
		return nullptr;
	}

	// Build diff line indices
	g_session.diffLineIndices.clear();
	for (int i = 0; i < (int)g_alignedLines.size(); ++i) {
		if (g_alignedLines[i].type != LineType::MATCH)
			g_session.diffLineIndices.push_back(i);
	}

	// Console width
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
	int consoleWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	if (consoleWidth < 40) consoleWidth = 80;
	g_session.halfWidth = consoleWidth / 2;

	g_session.tempFile = WriteTempFile(g_alignedLines, g_session.halfWidth);
	if (g_session.tempFile.empty()) return nullptr;

	g_session.active = true;

	auto getFileName = [](const std::wstring& path) -> std::wstring {
		auto pos = path.find_last_of(L"\\/");
		return (pos != std::wstring::npos) ? path.substr(pos + 1) : path;
	};
	std::wstring title = getFileName(file1) + L" \x2502 " + getFileName(file2);

	PsInfo.Editor(g_session.tempFile.c_str(),
		title.c_str(),
		0, 0, -1, -1,
		EF_NONMODAL | EF_IMMEDIATERETURN | EF_DISABLEHISTORY | EF_LOCKED,
		0, 0, CP_UTF8);

	return nullptr;
}

intptr_t WINAPI ProcessEditorEventW(const ProcessEditorEventInfo *Info)
{
	try {
	if (!g_session.active) return 0;

	if (Info->Event == EE_READ) {
		EditorInfo ei = { sizeof(EditorInfo) };
		PsInfo.EditorControl(-1, ECTL_GETINFO, 0, &ei);

		size_t fnSize = PsInfo.EditorControl(-1, ECTL_GETFILENAME, 0, nullptr);
		std::wstring fn(fnSize, 0);
		PsInfo.EditorControl(-1, ECTL_GETFILENAME, fnSize, &fn[0]);
		fn.resize(fnSize > 0 ? fnSize - 1 : 0);

		if (fn == g_session.tempFile) {
			g_session.editorId = ei.EditorID;
			int hw = g_session.halfWidth - 1;

			// Apply persistent colors (not AUTODELETE) for all lines
			for (intptr_t i = 0; i < (intptr_t)g_alignedLines.size(); ++i) {
				const auto& al = g_alignedLines[i];

				// Separator column
				EditorColor ec = {};
				ec.StructSize = sizeof(EditorColor);
				ec.StringNumber = i;
				ec.ColorItem = 0;
				ec.StartPos = hw;
				ec.EndPos = hw;
				ec.Priority = 0xFFFFFFFFU;
				ec.Flags = 0; // persistent!
				ec.Color.Flags = FCF_FG_INDEX | FCF_BG_INDEX;
				ec.Color.ForegroundColor = MAKE_OPAQUE(0x09);
				ec.Color.BackgroundColor = MAKE_OPAQUE(0x01);
				ec.Owner = EditorColorGuid;
				PsInfo.EditorControl(-1, ECTL_ADDCOLOR, 0, &ec);

				if (al.type == LineType::MATCH) continue;

				COLORREF fgL, bgL, fgR, bgR;
				switch (al.type) {
				case LineType::DEL:
					fgL = MAKE_OPAQUE(0x0F); bgL = MAKE_OPAQUE(0x04); // white on red
					fgR = MAKE_OPAQUE(0x08); bgR = MAKE_OPAQUE(0x08); // gray on gray
					break;
				case LineType::INS:
					fgL = MAKE_OPAQUE(0x08); bgL = MAKE_OPAQUE(0x08); // gray on gray
					fgR = MAKE_OPAQUE(0x0F); bgR = MAKE_OPAQUE(0x02); // white on green
					break;
				case LineType::CHANGED:
					fgL = MAKE_OPAQUE(0x00); bgL = MAKE_OPAQUE(0x0E); // black on yellow
					fgR = MAKE_OPAQUE(0x00); bgR = MAKE_OPAQUE(0x0E);
					break;
				default: continue;
				}

				// Left half
				EditorColor ecL = {};
				ecL.StructSize = sizeof(EditorColor);
				ecL.StringNumber = i;
				ecL.ColorItem = 0;
				ecL.StartPos = 0;
				ecL.EndPos = hw - 1;
				ecL.Priority = 0xFFFFFFFFU;
				ecL.Flags = 0; // persistent!
				ecL.Color.Flags = FCF_FG_INDEX | FCF_BG_INDEX;
				ecL.Color.ForegroundColor = fgL;
				ecL.Color.BackgroundColor = bgL;
				ecL.Owner = EditorColorGuid;
				PsInfo.EditorControl(-1, ECTL_ADDCOLOR, 0, &ecL);

				// Right half
				EditorColor ecR = {};
				ecR.StructSize = sizeof(EditorColor);
				ecR.StringNumber = i;
				ecR.ColorItem = 0;
				ecR.StartPos = hw + 1;
				ecR.EndPos = 2 * hw;
				ecR.Priority = 0xFFFFFFFFU;
				ecR.Flags = 0; // persistent!
				ecR.Color.Flags = FCF_FG_INDEX | FCF_BG_INDEX;
				ecR.Color.ForegroundColor = fgR;
				ecR.Color.BackgroundColor = bgR;
				ecR.Owner = EditorColorGuid;
				PsInfo.EditorControl(-1, ECTL_ADDCOLOR, 0, &ecR);

				// Character-level highlighting for CHANGED lines
				if (al.type == LineType::CHANGED) {
					EditorColor ecH = {};
					ecH.StructSize = sizeof(EditorColor);
					ecH.StringNumber = i;
					ecH.ColorItem = 0;
					ecH.Priority = 0xFFFFFFFFU;
					ecH.Flags = 0;
					ecH.Color.Flags = FCF_FG_INDEX | FCF_BG_INDEX;
					ecH.Owner = EditorColorGuid;

					// Highlight changed chars on left side (white on red)
					ecH.Color.ForegroundColor = MAKE_OPAQUE(0x0F);
					ecH.Color.BackgroundColor = MAKE_OPAQUE(0x04);
					for (auto& r : al.leftDiffRanges) {
						ecH.StartPos = r.first;       // offset within left text
						ecH.EndPos = r.second - 1;
						if (ecH.StartPos < hw && ecH.EndPos >= 0) {
							if (ecH.EndPos >= hw) ecH.EndPos = hw - 1;
							PsInfo.EditorControl(-1, ECTL_ADDCOLOR, 0, &ecH);
						}
					}

					// Highlight changed chars on right side (white on green)
					ecH.Color.ForegroundColor = MAKE_OPAQUE(0x0F);
					ecH.Color.BackgroundColor = MAKE_OPAQUE(0x02);
					for (auto& r : al.rightDiffRanges) {
						ecH.StartPos = hw + 1 + r.first;  // offset into right half
						ecH.EndPos = hw + 1 + r.second - 1;
						if (ecH.EndPos <= 2 * hw) {
							PsInfo.EditorControl(-1, ECTL_ADDCOLOR, 0, &ecH);
						}
					}
				}
			}
		}
	}
	else if (Info->Event == EE_CLOSE) {
		EditorInfo ei = { sizeof(EditorInfo) };
		PsInfo.EditorControl(-1, ECTL_GETINFO, 0, &ei);
		if (ei.EditorID == g_session.editorId) {
			CleanupSession();
			g_alignedLines.clear();
		}
	}

	} catch (...) {
		g_session.active = false;
	}
	return 0;
}
