// =====================================================================
//  自定义塔防小游戏 · 启动器主界面  (PvZ Launcher)
//  -------------------------------------------------------------------
//  技术 : C++17 / Win32 API / GDI+   （单文件，零第三方依赖）
//  风格 : 深色 · 简约 · 高级感（悬停动效 / 双缓冲无闪烁 / DPI 自适应）
//
//  MinGW 编译 :
//    g++ main.cpp -o PvZLauncher.exe -std=c++17 -O2 -municode -mwindows
//        -lgdiplus -lwinmm -lshell32 -lEasyXw
//        -static-libgcc -static-libstdc++
//
//  MSVC 编译 :
//    cl /std:c++17 /utf-8 /EHsc /O2 main.cpp
//        /link /SUBSYSTEM:WINDOWS gdiplus.lib user32.lib gdi32.lib
//             winmm.lib shell32.lib EasyXw.lib
//
//  ═══════════════════════════════════════════════════════════════════
//   【接口位置总览】 后续游戏逻辑从这里接入（Ctrl+F 搜索"接口位置"）：
//  ═══════════════════════════════════════════════════════════════════
//   [1] OnStartGameClicked()        开始游戏 → 搜索「接口位置 1」
//   [2] OnDesignCharacterClicked()   设计角色 → 搜索「接口位置 2」
//       （点击后弹出「三类生物」配置弹窗，弹窗本体搜索「弹窗位置」）
//   [3] OnXiaoHaoClicked()           小豪 → 启动同目录 demo.py（智能体工作台）
//   * 新增按钮 : 在 CreateButtons() 里复制一段 MenuButton 即可。
//   * 按钮外观 : DrawButton() 中统一绘制，改颜色/圆角/尺寸都在那里。
//   * DPI 适配 : 界面按 960x640 逻辑稿设计，布局时统一乘 g_scale，
//     不使用 GDI+ 世界变换（transform），兼容性最好。
// =====================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include <windows.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>    // ShellExecuteW / SearchPathW（「小豪」按钮启动 demo.py）
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void GameMain();
bool SelectEntityImages(std::vector<std::wstring>& outPaths, const wchar_t* title, int maxCount);

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

using namespace Gdiplus;

static HWND         g_mainHwnd = nullptr;     // 启动器窗口句柄（前置声明）
// 角色设计子菜单窗口句柄。必须在 DesignPopupWndProc / OpenDesignCharacterPopup
// 之前声明 —— 这两个函数位于本文件中更靠上的位置，弹窗关闭时要靠它决定
// 是回到子菜单还是回到启动器，放在后面会导致"未声明"的编译错误。
static HWND         g_charMenuHwnd = nullptr;

static void SetAssetWorkingDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    std::wstring directory(modulePath, length);
    const size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return;
    directory.resize(separator);

    std::wstring candidate = directory;
    for (int level = 0; level < 3 && !candidate.empty(); ++level) {
        const std::wstring imageDirectory = candidate + L"\\img";
        const DWORD attributes = GetFileAttributesW(imageDirectory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            SetCurrentDirectoryW(candidate.c_str());
            return;
        }

        const size_t parentSeparator = candidate.find_last_of(L"\\/");
        if (parentSeparator == std::wstring::npos) break;
        candidate.resize(parentSeparator);
    }
}

// ---------------------------------------------------------------------
// 设计基准：窗口逻辑稿 960 x 640，实际绘制 = 逻辑值 × g_scale
// ---------------------------------------------------------------------
static constexpr float kDesignW = 960.f;
static constexpr float kDesignH = 640.f;

enum class AppMode {
    Launcher,
    Game
};

static float        g_scale = 1.f;   // DPI 缩放系数
static AppMode      g_mode = AppMode::Launcher;
static std::wstring g_status;        // 底部状态栏文字

struct Plant {
    RectF rect{};
    int hp = 3;
};

struct Zombie {
    RectF rect{};
    float speed = 18.f;
    int hp = 1;
};

static float g_gameClock = 0.f;
static float g_spawnCooldown = 0.f;
static int   g_sun = 150;
static std::vector<Plant> g_plants;
static std::vector<Zombie> g_zombies;

// ---- 出战卡组 / 英雄携带（启动器配置，game.cpp 只读） ----
int  g_deckTypes[8] = { 0 };   // 用户选中的 8 张卡对应的 type（顺序即卡序）
int  g_deckCount   = 0;        // 实际选中数量（0 = 未配置，game 里回退为「全部可用」）
bool g_heroEnabled = false;    // 是否携带英雄

// 英雄每个状态 8 帧（攻击 / 休闲各一套，以用户上传的 8 张图片为准）。
// 定义在 main.cpp：UI（设计弹窗）要用，且 game.cpp 被 #include 进本文件、可见。
const int HERO_FRAMES = 8;

// 用户最多可添加的英雄数量（设计角色 → 英雄 页签里可切换 1~13 号英雄，
// 每个英雄各自一套「攻击 8 帧 / 休闲 8 帧 / 卡面」和一套数值）。
// 定义在 main.cpp：两处共用，且 game.cpp 被 #include 进本文件、可见。
const int MAX_HEROES = 13;

// 敌人动画帧数（设计角色 → 敌人生物 页签里可切换 1~13 号敌人，
// 每名敌人各自一套「行走 6 帧 / 攻击 8 帧」和一套数值：血量 + 伤害，无费用）。
// 定义在 main.cpp：UI 要用，且 game.cpp 被 #include 进本文件、可见。
const int ENEMY_WALK_FRAMES   = 6;
const int ENEMY_ATTACK_FRAMES = 8;

// ---- 出战卡组弹窗（由「角色设计 → 出战卡组」打开） ----
// 卡牌清单由 game.cpp 的 CollectDeckCardInfos() 枚举（内置 + 导入角色 + 英雄），
// 这里只负责勾选界面与 deck.txt 的读写。
struct DeckCardInfo {
    int      type;            // 与 game.cpp 的 type 体系一致（内置 1.. / 英雄 301..313）
    wchar_t  name[40];        // 显示名
    int      cost;            // 费用（阳光）
    wchar_t  imgPath[MAX_PATH]; // 卡面缩略图路径（空串=无图）
};
static DeckCardInfo            g_deckCards[64];
static int                    g_deckCardCount = 0;
static std::vector<bool>      g_deckPicked;          // 与 g_deckCards 一一对应
static bool                   g_deckHeroPicked = false;  // 是否携带英雄
static bool                   g_deckHeroAvail  = false;  // 英雄图是否已上传（开关可用性）
static std::vector<std::unique_ptr<Bitmap>> g_deckThumbs; // 缩略图（与 g_deckCards 对应）
static HWND                   g_deckHwnd = nullptr;
static int                    g_deckHoverTile = -1;
static int                    g_deckHoverHero = -1;
static std::wstring           g_deckTip;

// --------------------------- 字体 -------------------------------------
// 注意：GDI+ 的 Font 不会持有 FontFamily 的引用，FontFamily 必须比所有
// Font 活得更久，否则 DrawString 会访问已释放内存（乱码/错位/崩溃）。
// 字号在创建时直接乘以 g_scale（物理像素），绘制路径零变换。
static FontFamily* g_family = nullptr;

struct Fonts {
    std::unique_ptr<Font> title;   // 主标题
    std::unique_ptr<Font> sub;     // 英文副标题
    std::unique_ptr<Font> button;  // 按钮文字
    std::unique_ptr<Font> chev;    // 按钮右侧箭头
    std::unique_ptr<Font> status;  // 状态栏
    std::unique_ptr<Font> footer;  // 页脚
};
static Fonts g_fonts;

// ---------------------------------------------------------------------
// 前置声明
// ---------------------------------------------------------------------
static void SetActionStatus(const std::wstring& label, const std::wstring& fn);
static void OnStartGameClicked();
static void OnDesignCharacterClicked();
static void OnXiaoHaoClicked();
static void OpenDesignCharacterPopup();
static void ShowLauncherWindow();
// 角色设计子菜单 + 已有卡牌弹窗（本轮新增）
static void ShowCharMenuWindow();
static void OpenCharMenu();
static void OpenCardsPopup();
static void OnCharMenuDeck();       // 角色设计子菜单「出战卡组」
static void OpenDeckPopup();        // 出战卡组弹窗
static void LoadDeckFromDisk();     // 启动时读 deck.txt 到 g_deckTypes/g_deckCount/g_heroEnabled
static void RefreshDeck();          // 出战卡组弹窗重绘
static bool LaunchDemoPyAndWait(HWND hideHwnd, HWND errOwner);

// game.cpp 提供：枚举当前所有可用卡牌（内置 + 导入角色 + 英雄），供出战卡组弹窗勾选
// 注意：game.cpp 被 #include 进 main.cpp，二者同属一个翻译单元，
// 因此此处的声明不可加 static（否则与 game.cpp 里的非 static 定义链接冲突）。
int CollectDeckCardInfos(DeckCardInfo* out, int maxOut);

// 英雄图（两套各 8 帧）落盘 / 回读：定义见下方，先声明以满足前面弹窗的调用
static bool ApplyHeroToDisk(int& written, int& failed);
static void LoadHeroFromDisk();

// ---------------------------------------------------------------------
// 按钮模型
// ---------------------------------------------------------------------
struct MenuButton {
    std::wstring label;        // 按钮文字
    std::wstring funcName;     // 对应接口函数名（状态栏展示用）
    int          accent;       // 图标配色索引（见 kAccents）
    int          icon;         // 图标样式：0=播放（开始游戏） 1=角色（设计角色） 2=星芒（小豪）
    RectF        rect{};       // 物理像素坐标
    float        hover  = 0.f; // 悬停动画 0→1
    bool         over   = false;
    bool         pressed = false;
    std::function<void()> onClick;
};

static std::vector<MenuButton> g_buttons;

// 按钮图标基色（低饱和、同明度，保持整体克制）
static const Color kAccents[1] = {
    Color(255, 122, 162, 220),  // 雾蓝（开始游戏 / 设计角色共用同一风格）
};

static void CreateButtons(float s) {
    g_buttons.clear();
    const float bw = 360.f * s, bh = 60.f * s, gap = 20.f * s;
    const float x  = (kDesignW * s - bw) * 0.5f;
    const float y0 = 272.f * s;   // 三个按钮后整体上移，保持版面垂直居中

    auto make = [=](const wchar_t* label, const wchar_t* fn, int accent, int icon,
                    float y, void (*cb)()) {
        MenuButton b;
        b.label    = label;
        b.funcName = fn;
        b.accent   = accent;
        b.icon     = icon;
        b.rect     = RectF(x, y, bw, bh);
        b.onClick  = cb;
        g_buttons.push_back(std::move(b));
    };

    // 按钮一：开始游戏  /  按钮二：设计角色  /  按钮三：小豪（跳转 demo.py）
    // 新增按钮照抄一行 make(...) 即可；icon 取 0/1/2 选图标样式
    make(L"开始游戏", L"OnStartGameClicked",       0, 0, y0,                 &OnStartGameClicked);
    make(L"设计角色", L"OnDesignCharacterClicked", 0, 1, y0 + bh + gap,      &OnDesignCharacterClicked);
    make(L"小豪",     L"OnXiaoHaoClicked",         0, 2, y0 + (bh + gap)*2,  &OnXiaoHaoClicked);
}

// ---------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------
static float S(float v) { return v * g_scale; }   // 逻辑值 → 物理像素

static Color LerpColor(const Color& a, const Color& b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    auto mix = [&](BYTE x, BYTE y) {
        return (BYTE)std::lround(x + (float(y) - float(x)) * t);
    };
    return Color(mix(a.GetA(), b.GetA()), mix(a.GetR(), b.GetR()),
                 mix(a.GetG(), b.GetG()), mix(a.GetB(), b.GetB()));
}

static void AddRoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2.f;
    p.AddArc(x, y, d, d, 180.f, 90.f);
    p.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    p.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

static bool RectsOverlap(const RectF& a, const RectF& b) {
    return a.X < b.X + b.Width && a.X + a.Width > b.X &&
           a.Y < b.Y + b.Height && a.Y + a.Height > b.Y;
}

static void DrawTextInRect(Graphics& g, const std::wstring& s, const RectF& r,
                           Font* f, const Color& c,
                           StringAlignment align = StringAlignmentCenter) {
    SolidBrush br(c);
    StringFormat sf;
    sf.SetAlignment(align);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &sf, &br);
}

// =====================================================================
// 【接口位置 1】开始游戏 —— 游戏主循环入口
// ---------------------------------------------------------------------
// 后续实现 : 在此初始化游戏引擎并进入主游戏场景。
//   示例 : Game::GetInstance()->Start();        // 启动游戏主循环
//          SceneManager::SwitchTo(Scene::Game); // 或切换到游戏场景
//   也可以在此创建游戏窗口 / 加载存档 / 播放过场动画等。
// =====================================================================
static void ShowLauncherWindow() {
    g_mode = AppMode::Launcher;
    if (g_mainHwnd) {
        ShowWindow(g_mainHwnd, SW_SHOW);
        SetForegroundWindow(g_mainHwnd);
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

static void OnStartGameClicked() {
    g_mode = AppMode::Game;
    SetActionStatus(L"开始游戏", L"OnStartGameClicked");

    // 不要销毁启动器窗口，否则 WM_DESTROY 会触发 PostQuitMessage(0)，导致进程退出，无法进入游戏主体。
    // 这里切到游戏状态，并隐藏启动器窗口，让 game.cpp 里的 EasyX 主体继续运行。
    if (g_mainHwnd) {
        ShowWindow(g_mainHwnd, SW_HIDE);
    }

    GameMain();
    if (g_mainHwnd) {
        InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

// =====================================================================
// 【接口位置 2】设计角色 —— 智能体工作区
// ---------------------------------------------------------------------
// 后续实现 : 用户进入。智能体
//   示例 : 
// =====================================================================
static void OnDesignCharacterClicked() {
    SetActionStatus(L"角色设计", L"OnDesignCharacterClicked");
    OpenCharMenu();   // 打开 4 按钮子菜单（已有卡牌 / 添加角色 / 智能体 / 返回主菜单）
}

// =====================================================================
// 【弹窗位置】设计角色弹窗 —— 窗口骨架已就绪，内容区由你自己实现
// ---------------------------------------------------------------------
//   * 弹窗尺寸 / 标题 : OpenDesignCharacterPopup() 里的 kPopupW / kPopupH
//   * 弹窗内容绘制    : DrawDesignPopupScene() 中「TODO(弹窗内容)」注释处
//   * 鼠标 / 键盘消息 : DesignPopupWndProc() 中对应的 case
//   * 交互方式        : 弹窗跟随主窗口显示；点右上角 × 或按 Esc 关闭，
//                       不结束程序，主界面不受影响
// =====================================================================
// =====================================================================
// 【弹窗位置】设计角色弹窗 —— 敌人生物 + 英雄配置工坊
// ---------------------------------------------------------------------
//   两个页签（防御生物 / 攻击生物已移除，其落盘文件保留在磁盘但不再使用）：
//     敌人生物 : 进入游戏后作为敌对生物登场，最多 13 名，
//                每名一套「行走 6 帧 / 攻击 8 帧」+ 1 张「敌方卡牌图片」（可选，作封面）
//     英雄     : 最多 13 个英雄，每个英雄一套「攻击 8 帧 / 休闲 8 帧 / 卡面 / 子弹」
//
//   落盘约定（启动器写 / game.cpp 读，都用固定文件名，无需解析配置文件）：
//     img/custom/enemy1_walk_1.png   ... enemy13_walk_6.png      行走动画（每名 6 张）
//     img/custom/enemy1_attack_1.png ... enemy13_attack_8.png    攻击动画（每名 8 张）
//     img/custom/enemy1_card.png     ... enemy13_card.png        敌方卡牌封面（每名 1 张，可选）
//     img/custom/hero1_attack_1.png ... hero13_attack_8.png
//     img/custom/hero1_idle_1.png   ... hero13_idle_8.png
//     img/custom/hero1_card.png     ... hero13_card.png
//     img/custom/hero1_bullet.png   ... hero13_bullet.png         子弹图（配了 = 远程）
//   （旧版单图 enemy_N.png / hero_attack_N / hero_idle_N / hero_card.png 首次打开时
//     自动并入对应编号，只作只读兼容，保存后迁移到新命名并删掉旧文件）
//   （旧版的 hero_attack_N / hero_idle_N / hero_card.png 首次打开时自动并入 1 号英雄）
//   统一转成 PNG（GDI+ 编码，保留透明通道）并按类别限制长边，避免游戏侧读图过大。
//   * 弹窗尺寸 / 标题 : OpenDesignCharacterPopup() 里的 kPopupW / kPopupH
//   * 弹窗内容绘制    : DrawDesignPopupScene()
//   * 鼠标 / 键盘消息 : DesignPopupWndProc()
//   * 交互方式        : 弹窗跟随主窗口显示；点右上角 × 或按 Esc 关闭，
//                       不结束程序，主界面不受影响
// =====================================================================
static HWND            g_designPopup = nullptr;                 // 弹窗句柄（防重复打开）
static std::wstring    g_importedEntityPath;                    // 兼容保留：旧版单图导入路径（新界面不再写入）
static std::unique_ptr<Bitmap> g_importedEntityPreview;         // 兼容保留
static const wchar_t*  kPopupClass   = L"PvZDesignPopupWnd";    // 弹窗窗口类名

// ---- 三类生物的数据模型 -------------------------------------------
static constexpr int kMaxBioSlots = 13;   // 敌人最多 13 名（改这里即可扩容）
static constexpr int kBioColumns  = 7;    // 已有卡牌弹窗的缩略图网格列数（13 张 → 2 行）
static constexpr int kTabCount    = 1;    // 页签数量（防御 / 攻击生物已移除，只剩敌人生物）

enum BioCategory { BIO_ENEMY = 0 };

struct BioCategoryInfo {
    const wchar_t* name;      // 页签名（带数量显示）
    const wchar_t* hint;      // 页签下的一句话说明
    const wchar_t* fileName;  // 落盘文件名前缀
    int            maxSide;   // 保存时限定的长边像素
};

static const BioCategoryInfo kBioInfo[kTabCount] = {
    { L"敌人生物", L"作为敌对生物从右侧登场，会啃食路径上的生物；最多 13 名，每名一套「行走 6 帧 + 攻击 8 帧」+ 1 张卡牌封面",
      L"enemy",   200 },
};

// =====================================================================
// 生物数值：血量 / 费用 / 伤害
//   每个分类的每个槽位单独一套数值，在弹窗里编辑后写进 img\custom\stats.ini；
//   游戏启动时 game.cpp 读同一个文件并真正生效（见 game.cpp 的 LoadCustomStats）。
//   文件内容全是 ASCII（分类名 + 数字），所以用窄字符流读写，避开宽字符编码坑。
// =====================================================================
struct BioStat {
    int hp;    // 血量：防御/攻击 = 能被啃多久；敌人 = 挨多少点伤害才倒下；英雄 = 扛多少伤害
    int cost;  // 费用：从工具栏放下要花多少阳光（敌人不需要放置；英雄默认 0=携带免费）
    int dmg;   // 伤害：攻击 = 每颗子弹打掉多少血；敌人 = 每次啃食造成多少伤害；英雄 = 近战每刀伤害
};

// 英雄数值（血量 / 费用 / 伤害）：设计角色 → 英雄 里按英雄编号分别配置，
// 存于 stats.ini 的 [hero] 段（1..13 行）。全局定义在这里（BioStat 之后），
// game.cpp 通过 extern 引用。
BioStat g_heroStat[MAX_HEROES];

// 英雄数值默认值：血量 300、费用 0（携带免费）、伤害 40
static void ResetHeroStats() {
    for (int i = 0; i < MAX_HEROES; ++i) g_heroStat[i] = BioStat{ 300, 0, 40 };
}

// 数值索引，和界面上的三组编辑器一一对应
enum { STAT_HP = 0, STAT_COST = 1, STAT_DMG = 2, STAT_COUNT };

static const wchar_t* kStatNames[STAT_COUNT]  = { L"血量", L"费用", L"伤害" };
static const int      kStatStep[STAT_COUNT]   = { 10, 10, 5 };      // 每次 +／− 的步长
static const int      kStatMin[STAT_COUNT]    = { 1, 0, 0 };
static const int      kStatMax[STAT_COUNT]    = { 9999, 9990, 999 };

static BioStat g_bioStats[kTabCount][kMaxBioSlots];

// 各类生物开放哪些数值：敌人不需要放置 → 没费用；伤害 = 每次啃食造成的伤害
// （英雄三项全开，在调用处单独判断，见 kHeroTab 分支）
static bool BioHasStat(int category, int which) {
    if (which == STAT_HP)   return true;
    if (which == STAT_COST) return false;   // 敌人生物不需要放置 → 没费用
    if (which == STAT_DMG)  return true;    // 敌人的伤害 = 每次啃食造成的伤害
    return false;
}

static int* BioStatField(int category, int slot, int which) {
    if (category < 0 || category >= kTabCount) return nullptr;
    if (slot < 0 || slot >= kMaxBioSlots) return nullptr;
    BioStat& s = g_bioStats[category][slot];
    if (which == STAT_HP)   return &s.hp;
    if (which == STAT_COST) return &s.cost;
    if (which == STAT_DMG)  return &s.dmg;
    return nullptr;
}

static void ResetBioStats() {
    for (int c = 0; c < kTabCount; ++c) {
        for (int i = 0; i < kMaxBioSlots; ++i) {
            BioStat s;
            s.hp   = 100;
            s.cost = 100;
            // 伤害按"每次结算扣多少血"计价（结算与主循环同频，约 20ms 一次）：
            //   敌人 2  → 约 1 秒啃掉一株 100 血的植物（默认僵尸是 1，约 2 秒）
            s.dmg  = 2;
            g_bioStats[c][i] = s;
        }
    }
    ResetHeroStats();   // 英雄数值（13 套）也回到默认
}

static const char* kBioStatsFile = "img\\custom\\stats.ini";   // 纯 ASCII 内容

static bool SaveBioStats() {
    CreateDirectoryW(L"img", nullptr);
    CreateDirectoryW(L"img\\custom", nullptr);
    std::ofstream out(kBioStatsFile, std::ios::out | std::ios::trunc);
    if (!out) return false;
    const char* names[kTabCount] = { "enemy" };
    for (int c = 0; c < kTabCount; ++c) {
        out << "[" << names[c] << "]\n";
        for (int i = 0; i < kMaxBioSlots; ++i) {
            const BioStat& s = g_bioStats[c][i];
            out << (i + 1) << "=" << s.hp << "," << s.cost << "," << s.dmg << "\n";
        }
    }
    // 英雄数值：每个英雄一套（hp,cost,dmg），行号 = 英雄编号 1..13
    out << "[hero]\n";
    for (int i = 0; i < MAX_HEROES; ++i) {
        const BioStat& s = g_heroStat[i];
        out << (i + 1) << "=" << s.hp << "," << s.cost << "," << s.dmg << "\n";
    }
    return out.good();
}

static bool LoadBioStats() {
    std::ifstream in(kBioStatsFile);
    if (!in) return false;
    const char* names[kTabCount] = { "enemy" };
    int cat = -1;
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        while (!line.empty() && (unsigned char)line.back() <= ' ') line.pop_back();   // 去尾部空格/CR
        size_t begin = 0;
        while (begin < line.size() && (unsigned char)line[begin] <= ' ') ++begin;      // 去行首空格
        if (begin > 0) line.erase(0, begin);
        // 首行可能带 UTF-8 BOM（用记事本另存过就会有），不去掉会导致整个 [分类] 识别不到
        if (firstLine && line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        firstLine = false;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t end = line.find(']');
            if (end == std::string::npos) continue;
            const std::string name = line.substr(1, end - 1);
            cat = -1;
            if (name == "hero") { cat = 99; continue; }     // 英雄数值段（单套）
            for (int c = 0; c < kTabCount; ++c) if (name == names[c]) { cat = c; break; }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || cat < 0) continue;
        const int index = std::atoi(line.substr(0, eq).c_str());
        if (index < 1 || index > kMaxBioSlots) continue;

        // 解析 "血量,费用,伤害"：不用 sscanf（MSVC 会报 C4996 不安全），
        // 自己按逗号切分，字段前后允许空格；缺的字段保持 ResetBioStats 给的默认值，
        // 这样旧版只写了 2 个字段的 stats.ini 也还能读，不会整行作废。
        int values[STAT_COUNT] = { 0, 0, 0 };
        int got = 0;
        const char* cursor = line.c_str() + eq + 1;
        for (int f = 0; f < STAT_COUNT; ++f) {
            while (*cursor == ' ' || *cursor == '\t') ++cursor;      // 跳过字段前的空格
            char* end = nullptr;
            const long v = std::strtol(cursor, &end, 10);
            if (end == cursor) break;                                // 不是数字 → 后面都不认
            values[f] = (v > 9999) ? 9999 : (v < -9999 ? -9999 : (int)v);
            got = f + 1;
            cursor = end;
            while (*cursor == ' ' || *cursor == '\t') ++cursor;
            if (*cursor != ',') break;                               // 没有下一个字段了
            ++cursor;                                                // 吃掉逗号
        }
        if (got < 1) continue;                                       // 一个数字都没有 → 跳过

        if (cat == 99) {                                             // 英雄数值段（行号 = 英雄编号）
            BioStat& hs = g_heroStat[index - 1];
            if (got >= 1) hs.hp   = std::clamp(values[STAT_HP],   kStatMin[STAT_HP],   kStatMax[STAT_HP]);
            if (got >= 2) hs.cost = std::clamp(values[STAT_COST], kStatMin[STAT_COST], kStatMax[STAT_COST]);
            if (got >= 3) hs.dmg  = std::clamp(values[STAT_DMG],  kStatMin[STAT_DMG],  kStatMax[STAT_DMG]);
            continue;
        }

        BioStat& s = g_bioStats[cat][index - 1];
        if (got >= 1) s.hp   = std::clamp(values[STAT_HP],   kStatMin[STAT_HP],   kStatMax[STAT_HP]);
        if (got >= 2) s.cost = std::clamp(values[STAT_COST], kStatMin[STAT_COST], kStatMax[STAT_COST]);
        if (got >= 3) s.dmg  = std::clamp(values[STAT_DMG],  kStatMin[STAT_DMG],  kStatMax[STAT_DMG]);
    }
    return true;
}

// ---- 敌人（最多 13 名）：每名敌人一套「行走 6 帧 / 攻击 8 帧」----
static constexpr int EnemyFrameTotal(int which) {
    return (which == 0) ? ENEMY_WALK_FRAMES : ENEMY_ATTACK_FRAMES;
}

struct EnemyData {
    std::vector<std::wstring>            walkPaths;    // 行走动画源图路径（最多 6 张）
    std::vector<std::unique_ptr<Bitmap>> walkThumbs;
    std::vector<std::wstring>            attackPaths;  // 攻击动画源图路径（最多 8 张）
    std::vector<std::unique_ptr<Bitmap>> attackThumbs;
    std::wstring                         cardPath;     // 敌方卡面（1 张，「已有卡牌 → 敌人卡牌」封面）
    std::unique_ptr<Bitmap>              cardThumb;
};
static EnemyData g_enemies[kMaxBioSlots];

// 当前正在编辑的敌人编号（0..12 → 界面上的 1..13 号）
static int  g_enemyIndex      = 0;
static int  g_enemyHoverSlot  = -1;    // 悬停的敌人图槽（-1=无）
static int  g_enemyHoverWhich = -1;    // 悬停的是哪套：0=行走 1=攻击
static int  g_enemyHoverPick  = -1;    // 悬停的敌人编号按钮（-1=无）
static bool g_enemyHoverCard  = false; // 悬停敌人卡面槽

static EnemyData& CurEnemy() { return g_enemies[g_enemyIndex]; }

// 这名敌人是否已经配了图（有任意一帧或卡面就算「已添加」）
static bool EnemyConfigured(int index) {
    if (index < 0 || index >= kMaxBioSlots) return false;
    const EnemyData& e = g_enemies[index];
    return !e.walkPaths.empty() || !e.attackPaths.empty() || !e.cardPath.empty();
}

// 已添加的敌人数量（页签上显示 n/13）
static int EnemyConfiguredCount() {
    int n = 0;
    for (int i = 0; i < kMaxBioSlots; ++i) if (EnemyConfigured(i)) ++n;
    return n;
}

static int                                  g_bioTab = BIO_ENEMY;    // 当前页签
static int                                  g_hoverStatBtn = -1;     // 悬停的数值按钮：which*2+(0=−,1=+)，-1=无
static std::wstring                         g_popupTip;              // 弹窗底部状态提示

// ---- 英雄（最多 13 个）：每个英雄一套「攻击 8 帧 / 休闲 8 帧 / 卡面」----
//     落盘为 heroN_attack_n / heroN_idle_n / heroN_card.png（N = 英雄编号 1..13）
static constexpr int kHeroTab = 1;           // 设计弹窗里「英雄」页签的索引（排在敌人生物之后）

struct HeroData {
    std::vector<std::wstring>            attackPaths;   // 攻击状态源图路径（最多 8 张）
    std::vector<std::wstring>            idlePaths;     // 休闲状态源图路径（最多 8 张）
    std::vector<std::unique_ptr<Bitmap>> attackThumbs;  // 缩略图（长边 ≤256）
    std::vector<std::unique_ptr<Bitmap>> idleThumbs;
    std::wstring                         cardPath;      // 英雄卡面（工具栏图标）源图路径
    std::unique_ptr<Bitmap>              cardThumb;
    std::wstring                         bulletPath;    // 子弹图源图路径（配了 = 远程英雄）
    std::unique_ptr<Bitmap>              bulletThumb;
};
static HeroData g_heroes[MAX_HEROES];

static int  g_heroIndex      = 0;     // 当前正在编辑的英雄（0..12 → 界面上的 1..13 号）
static int  g_heroHoverSlot  = -1;    // 悬停的英雄图槽（-1=无）
static int  g_heroHoverWhich = -1;    // 悬停的是哪套：0=攻击 1=休闲
static int  g_heroHoverPick  = -1;    // 悬停的英雄编号按钮（-1=无）
static bool g_heroHoverCard  = false; // 悬停英雄卡面槽
static bool g_heroHoverBullet = false; // 悬停英雄子弹槽

// 这个英雄是不是远程（配了子弹图就是远程，否则近战）
static bool HeroIsRanged(int index) {
    if (index < 0 || index >= MAX_HEROES) return false;
    return !g_heroes[index].bulletPath.empty();
}

// 当前编辑的英雄（写在一处，避免到处写下标）
static HeroData& CurHero() { return g_heroes[g_heroIndex]; }

// 这个英雄是否已经配了图（有任意一帧或卡面就算「已添加」）
static bool HeroConfigured(int index) {
    if (index < 0 || index >= MAX_HEROES) return false;
    const HeroData& h = g_heroes[index];
    return !h.attackPaths.empty() || !h.idlePaths.empty() || !h.cardPath.empty();
}

// 已添加的英雄数量（页签上显示 n/13）
static int HeroConfiguredCount() {
    int n = 0;
    for (int i = 0; i < MAX_HEROES; ++i) if (HeroConfigured(i)) ++n;
    return n;
}

static constexpr float kPopupW   = 900.f;   // 弹窗逻辑宽（实际 = × g_scale）
static constexpr float kPopupH   = 720.f;   // 弹窗逻辑高（加「数值编辑区」后从 660 加高）
static constexpr float kSlotSide = 100.f;   // 缩略图槽边长（逻辑）
static constexpr float kSlotGap  = 12.f;    // 槽间距（逻辑）

// 英雄图网格：每套 8 张，单行 8 列；攻击在上（which=0）、休闲在下（which=1）
static RectF HeroSlotRect(int which, int i) {
    const float side = 68.f, gap = 8.f;
    const float gridW = HERO_FRAMES * side + (HERO_FRAMES - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    const float y = (which == 0) ? 240.f : 326.f;
    return RectF(S(x0 + i * (side + gap)), S(y), S(side), S(side));
}
static RectF HeroGridLabelRect(int which) {
    const float side = 68.f, gap = 8.f;
    const float gridW = HERO_FRAMES * side + (HERO_FRAMES - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    const float y = (which == 0) ? 222.f : 308.f;
    return RectF(S(x0), S(y), S(gridW), S(16.f));
}

// 英雄编号按钮（1..13）：点一下切换当前编辑的英雄
static RectF HeroPickRect(int index) {         // index 从 0 开始
    const float w = 52.f, gap = 6.f, h = 34.f;
    const float gridW = MAX_HEROES * w + (MAX_HEROES - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    return RectF(S(x0 + index * (w + gap)), S(178.f), S(w), S(h));
}

// 英雄图格右上角的移除角标（与 DrawBioSlot 里画的 30×30 角标保持一致）
static RectF HeroSlotRemoveRect(const RectF& slot) {
    return RectF(slot.X + slot.Width - S(30.f), slot.Y, S(30.f), S(30.f));
}

// ---- 弹窗版式（全部逻辑坐标，绘制时经 S() 乘 DPI 缩放）---------------
static RectF TabRect(int index) {
    return RectF(S(36.f + index * 208.f), S(100.f), S(196.f), S(46.f));
}

// 敌人图网格：行走 6 帧在上（which=0）、攻击 8 帧在下（which=1），格子与英雄页同尺寸
static RectF EnemySlotRect(int which, int i) {
    const float side = 68.f, gap = 8.f;
    const int frames = EnemyFrameTotal(which);
    const float gridW = frames * side + (frames - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    const float y = (which == 0) ? 240.f : 326.f;
    return RectF(S(x0 + i * (side + gap)), S(y), S(side), S(side));
}
static RectF EnemyGridLabelRect(int which) {
    const float side = 68.f, gap = 8.f;
    const int frames = EnemyFrameTotal(which);
    const float gridW = frames * side + (frames - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    const float y = (which == 0) ? 222.f : 308.f;
    return RectF(S(x0), S(y), S(gridW), S(16.f));
}

// 敌人编号按钮（1..13）：点一下切换当前编辑的敌人
static RectF EnemyPickRect(int index) {         // index 从 0 开始
    const float w = 52.f, gap = 6.f, h = 34.f;
    const float gridW = kMaxBioSlots * w + (kMaxBioSlots - 1) * gap;
    const float x0 = (kPopupW - gridW) * 0.5f;
    return RectF(S(x0 + index * (w + gap)), S(178.f), S(w), S(h));
}

static RectF SlotRemoveRect(const RectF& slot) {
    return RectF(slot.X + slot.Width - S(30.f), slot.Y, S(30.f), S(30.f));
}

// ---- 数值编辑区：选中一张图后，在这里改它的 血量 / 费用 / 伤害 ----
// 三组并排：每组 = [标签][−][数值][+]
static RectF StatGroupRect(int which) {
    const float gw = 258.f, gap = 18.f, x0 = 64.f, y = 446.f;
    return RectF(S(x0 + which * (gw + gap)), S(y), S(gw), S(42.f));
}
static RectF StatMinusRect(int which) {
    const RectF g = StatGroupRect(which);
    return RectF(g.X + S(68.f), g.Y + S(5.f), S(38.f), S(32.f));
}
static RectF StatValueRect(int which) {
    const RectF g = StatGroupRect(which);
    return RectF(g.X + S(110.f), g.Y + S(5.f), S(92.f), S(32.f));
}
static RectF StatPlusRect(int which) {
    const RectF g = StatGroupRect(which);
    return RectF(g.X + S(206.f), g.Y + S(5.f), S(38.f), S(32.f));
}

static RectF ApplyButtonRect() {
    return RectF(S(kPopupW - 36.f - 214.f), S(kPopupH - 64.f), S(214.f), S(50.f));
}
static RectF ClearButtonRect() {
    const RectF apply = ApplyButtonRect();
    return RectF(apply.X - S(12.f) - S(140.f), apply.Y, S(140.f), S(50.f));
}
static RectF CloseButtonRect() {
    const RectF clear = ClearButtonRect();
    return RectF(clear.X - S(12.f) - S(112.f), clear.Y, S(112.f), S(50.f));
}
static RectF TipTextRect() {
    return RectF(S(36.f), S(kPopupH - 92.f), S(kPopupW - 72.f), S(24.f));
}

// ---- 落盘路径 / 图片读写 ---------------------------------------------
// 敌人（enemyIdx 从 0 开始 = 界面上的 1 号敌人；frame 从 1 开始）
//   新版：enemy{N}_walk_{f}.png（f=1..6） / enemy{N}_attack_{f}.png（f=1..8）
//   旧版：enemy_{N}.png 只有一张静态图，只读兼容，当作「行走动画第 1 帧」
static std::wstring EnemyFilePath(int enemyIdx, int which, int frame) {
    return L"img\\custom\\enemy" + std::to_wstring(enemyIdx + 1) + L"_" +
           (which == 0 ? L"walk" : L"attack") + L"_" + std::to_wstring(frame) + L".png";
}
static std::wstring LegacyEnemyFilePath(int index) {   // index 从 1 开始
    return L"img\\custom\\enemy_" + std::to_wstring(index) + L".png";
}

static const std::wstring kStageDir       = L"img\\custom_stage";   // 落盘暂存目录

static std::wstring EnemyStageFilePath(int enemyIdx, int which, int frame) {
    return kStageDir + L"\\enemy" + std::to_wstring(enemyIdx + 1) + L"_" +
           (which == 0 ? L"walk" : L"attack") + L"_" + std::to_wstring(frame) + L".png";
}
// 敌方卡面（1 张）：enemy{N}_card.png —— 用于「已有卡牌 → 敌人卡牌」的封面
static std::wstring EnemyCardFilePath(int enemyIdx) {
    return L"img\\custom\\enemy" + std::to_wstring(enemyIdx + 1) + L"_card.png";
}
static std::wstring EnemyCardStageFilePath(int enemyIdx) {
    return kStageDir + L"\\enemy" + std::to_wstring(enemyIdx + 1) + L"_card.png";
}

// ---- 英雄图落盘路径（heroIdx 从 0 开始 = 界面上的 1 号英雄；frame 从 1 开始）----
//   新版（多英雄）：hero{N}_attack_{f}.png / hero{N}_idle_{f}.png / hero{N}_card.png
//   旧版（只有一个英雄时）：hero_attack_{f}.png / hero_idle_{f}.png / hero_card.png
//   旧版只读作兼容：打开弹窗 / 启动游戏时，新版文件不存在就退回旧文件名；
//   保存时会把 1 号英雄写成新版并清掉旧文件，完成一次性迁移。
static std::wstring HeroFilePath(int heroIdx, int which, int frame) {
    return L"img\\custom\\hero" + std::to_wstring(heroIdx + 1) + L"_" +
           (which == 0 ? L"attack" : L"idle") + L"_" + std::to_wstring(frame) + L".png";
}
static std::wstring HeroCardFilePath(int heroIdx) {
    return L"img\\custom\\hero" + std::to_wstring(heroIdx + 1) + L"_card.png";
}
static std::wstring HeroBulletFilePath(int heroIdx) {
    return L"img\\custom\\hero" + std::to_wstring(heroIdx + 1) + L"_bullet.png";
}
static std::wstring LegacyHeroFilePath(int which, int frame) {
    return std::wstring(L"img\\custom\\hero_") + (which == 0 ? L"attack" : L"idle") +
           L"_" + std::to_wstring(frame) + L".png";
}
static std::wstring LegacyHeroCardPath() {
    return L"img\\custom\\hero_card.png";
}

static bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 取该英雄这一帧「磁盘上真实存在」的文件（新版优先，1 号英雄兼容旧命名）
static bool HeroFramePath(int heroIdx, int which, int frame, std::wstring& out) {
    std::wstring path = HeroFilePath(heroIdx, which, frame);
    if (!PathExists(path) && heroIdx == 0) {
        const std::wstring legacy = LegacyHeroFilePath(which, frame);
        if (PathExists(legacy)) path = legacy;
    }
    if (!PathExists(path)) return false;
    out = path;
    return true;
}

// 该英雄的卡面文件（同样兼容 1 号英雄的旧命名）
static bool HeroCardPathOnDisk(int heroIdx, std::wstring& out) {
    std::wstring path = HeroCardFilePath(heroIdx);
    if (!PathExists(path) && heroIdx == 0) {
        const std::wstring legacy = LegacyHeroCardPath();
        if (PathExists(legacy)) path = legacy;
    }
    if (!PathExists(path)) return false;
    out = path;
    return true;
}
// 该英雄的子弹图文件（没有旧版兼容：子弹图是本次新增的功能）
static bool HeroBulletPathOnDisk(int heroIdx, std::wstring& out) {
    const std::wstring path = HeroBulletFilePath(heroIdx);
    if (!PathExists(path)) return false;
    out = path;
    return true;
}

// 取第 enemyIdx 名敌人这一帧「磁盘上真实存在」的文件（新版优先，兼容旧版单图）
static bool EnemyFramePath(int enemyIdx, int which, int frame, std::wstring& out) {
    std::wstring path = EnemyFilePath(enemyIdx, which, frame);
    if (!PathExists(path) && which == 0 && frame == 1) {
        const std::wstring legacy = LegacyEnemyFilePath(enemyIdx + 1);
        if (PathExists(legacy)) path = legacy;
    }
    if (!PathExists(path)) return false;
    out = path;
    return true;
}

// 第 enemyIdx 名敌人的卡面文件（没有旧版兼容：卡面是本次新增的功能）
static bool EnemyCardPathOnDisk(int enemyIdx, std::wstring& out) {
    const std::wstring path = EnemyCardFilePath(enemyIdx);
    if (!PathExists(path)) return false;
    out = path;
    return true;
}

// 读图 → 等比缩到长边 <= maxSide（只缩不放）→ 32bpp ARGB 位图（保留透明）
static std::unique_ptr<Bitmap> LoadScaledImage(const std::wstring& source, int maxSide) {
    std::unique_ptr<Bitmap> original(Bitmap::FromFile(source.c_str(), FALSE));
    if (!original || original->GetLastStatus() != Ok) return nullptr;
    const UINT sourceW = original->GetWidth(), sourceH = original->GetHeight();
    if (sourceW == 0 || sourceH == 0) return nullptr;

    float scale = 1.f;
    const float longest = (float)std::max(sourceW, sourceH);
    if (maxSide > 0 && longest > (float)maxSide) scale = (float)maxSide / longest;

    const UINT targetW = (UINT)std::max(1L, std::lround(sourceW * scale));
    const UINT targetH = (UINT)std::max(1L, std::lround(sourceH * scale));

    std::unique_ptr<Bitmap> scaled(new Bitmap(targetW, targetH, PixelFormat32bppARGB));
    if (!scaled || scaled->GetLastStatus() != Ok) return nullptr;
    {
        Graphics graphics(scaled.get());
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.DrawImage(original.get(), Rect(0, 0, (INT)targetW, (INT)targetH),
                           0, 0, (INT)sourceW, (INT)sourceH, UnitPixel);
    }
    return scaled;
}

static bool GetPngEncoderClsid(CLSID& clsid) {
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || bytes == 0) return false;
    std::vector<BYTE> buffer(bytes);
    ImageCodecInfo* codecs = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, bytes, codecs) != Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// 任意格式图片 → 缩放 → 存成 PNG（游戏侧用 EasyX loadimage 直接读）
static bool WritePngFromFile(const std::wstring& source, const std::wstring& target, int maxSide) {
    std::unique_ptr<Bitmap> image = LoadScaledImage(source, maxSide);
    if (!image) return false;
    CLSID pngClsid{};
    if (!GetPngEncoderClsid(pngClsid)) return false;
    return image->Save(target.c_str(), &pngClsid, nullptr) == Ok;
}

// 把 13 名敌人的动画帧写入 img/custom（多余帧的旧文件会删除）
//
// 注意：源图片有可能就是 img/custom 里的文件（打开弹窗时从磁盘回读），
// 所以必须「先全部重新编码到暂存目录，确认成功后再替换正式目录」，
// 否则先删文件会把源文件一起删掉，导致配置被清空。
static bool ApplyEnemiesToDisk(int& written, int& failed) {
    CreateDirectoryW(L"img", nullptr);
    CreateDirectoryW(L"img\\custom", nullptr);      // 正式目录：不存在时 MoveFileEx 会直接失败
    CreateDirectoryW(kStageDir.c_str(), nullptr);

    // 1) 清掉上一次可能残留的暂存文件（含卡面）
    for (int n = 0; n < kMaxBioSlots; ++n) {
        for (int which = 0; which < 2; ++which)
            for (int f = 1; f <= EnemyFrameTotal(which); ++f)
                DeleteFileW(EnemyStageFilePath(n, which, f).c_str());
        DeleteFileW(EnemyCardStageFilePath(n).c_str());
    }

    // 2) 全部重新编码到暂存目录（此时正式目录里的旧文件还完好）
    int requested = 0;
    for (int n = 0; n < kMaxBioSlots; ++n) {
        for (int which = 0; which < 2; ++which) {
            const auto& paths = (which == 0) ? g_enemies[n].walkPaths
                                              : g_enemies[n].attackPaths;
            requested += (int)paths.size();
            for (int i = 0; i < (int)paths.size() && i < EnemyFrameTotal(which); ++i) {
                if (WritePngFromFile(paths[i], EnemyStageFilePath(n, which, i + 1),
                                     kBioInfo[BIO_ENEMY].maxSide))
                    written++;
                else
                    failed++;
            }
        }
        // 敌方卡面（1 张，可选）
        if (!g_enemies[n].cardPath.empty()) {
            requested += 1;
            if (WritePngFromFile(g_enemies[n].cardPath, EnemyCardStageFilePath(n),
                                 kBioInfo[BIO_ENEMY].maxSide))
                written++;
            else
                failed++;
        }
    }

    // 3) 本来有内容却一张都没成功 → 判定整体失败，保留原配置不动（不做替换）
    if (requested > 0 && failed == requested) return false;

    // 4) 替换：先删正式目录里由本程序管理的文件（含旧版单图和卡面），再把暂存文件搬过去
    for (int n = 0; n < kMaxBioSlots; ++n) {
        for (int which = 0; which < 2; ++which)
            for (int f = 1; f <= EnemyFrameTotal(which); ++f)
                DeleteFileW(EnemyFilePath(n, which, f).c_str());
        DeleteFileW(LegacyEnemyFilePath(n + 1).c_str());
        DeleteFileW(EnemyCardFilePath(n).c_str());
    }
    for (int n = 0; n < kMaxBioSlots; ++n) {
        for (int which = 0; which < 2; ++which)
            for (int f = 1; f <= EnemyFrameTotal(which); ++f) {
                const std::wstring staged = EnemyStageFilePath(n, which, f);
                if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                MoveFileExW(staged.c_str(), EnemyFilePath(n, which, f).c_str(),
                            MOVEFILE_REPLACE_EXISTING);
            }
        const std::wstring stagedCard = EnemyCardStageFilePath(n);
        if (GetFileAttributesW(stagedCard.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        MoveFileExW(stagedCard.c_str(), EnemyCardFilePath(n).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    }
    return true;
}

// 保存并应用：敌人动画 + 英雄素材 + 数值，一次性落盘
static bool ApplyBiosToDisk(std::wstring& tip) {
    CreateDirectoryW(L"img", nullptr);
    CreateDirectoryW(L"img\\custom", nullptr);

    // 敌人：13 名 ×（行走 6 帧 / 攻击 8 帧 / 卡面 1 张）
    int enemyWritten = 0, enemyFailed = 0;
    ApplyEnemiesToDisk(enemyWritten, enemyFailed);

    // 数值一起落盘；写不进去不影响图片，但要告诉用户
    const bool statsSaved = SaveBioStats();

    // 英雄：两套各 8 帧 + 卡面 + 子弹图
    int heroWritten = 0, heroFailed = 0;
    ApplyHeroToDisk(heroWritten, heroFailed);

    RemoveDirectoryW(kStageDir.c_str());   // 目录空了才会删掉

    tip = L"已应用：敌人 " + std::to_wstring(EnemyConfiguredCount()) + L" 名";
    if (enemyWritten > 0) tip += L"（" + std::to_wstring(enemyWritten) + L" 帧）";
    tip += L" · 英雄 " + std::to_wstring(HeroConfiguredCount()) + L" 个";
    tip += L"（重新开始游戏后生效）";
    if (!statsSaved) tip += L"｜数值保存失败（stats.ini 无法写入）";
    if (enemyFailed > 0) tip += L"｜有 " + std::to_wstring(enemyFailed) + L" 帧读取失败，已跳过";
    return enemyFailed == 0;
}

// 打开弹窗时回读 img/custom 下全部敌人（每名两套动画），与"当前已生效"的配置一致
static void LoadEnemiesFromDisk() {
    for (int n = 0; n < kMaxBioSlots; ++n) {
        EnemyData& enemy = g_enemies[n];
        enemy.walkPaths.clear();   enemy.walkThumbs.clear();
        enemy.attackPaths.clear(); enemy.attackThumbs.clear();
        enemy.cardPath.clear();    enemy.cardThumb.reset();

        for (int which = 0; which < 2; ++which) {
            auto& paths  = (which == 0) ? enemy.walkPaths   : enemy.attackPaths;
            auto& thumbs = (which == 0) ? enemy.walkThumbs  : enemy.attackThumbs;
            for (int frame = 1; frame <= EnemyFrameTotal(which); ++frame) {
                std::wstring path;
                if (!EnemyFramePath(n, which, frame, path)) break;
                std::unique_ptr<Bitmap> thumb = LoadScaledImage(path, 256);
                if (!thumb) break;
                paths.push_back(path);
                thumbs.push_back(std::move(thumb));
            }
        }
        // 敌方卡面（1 张，可选）：有则载入缩略图
        std::wstring card;
        if (EnemyCardPathOnDisk(n, card)) {
            enemy.cardPath = card;
            enemy.cardThumb = LoadScaledImage(card, 256);
        }
    }
}

// 打开弹窗时回读 img/custom，让界面反映"当前已生效"的配置
static void LoadBiosFromDisk() {
    // 数值：先给默认值，再用 stats.ini 覆盖（文件不存在就全用默认）
    ResetBioStats();
    LoadBioStats();

    // 敌人：回读 img/custom 下 enemyN_walk_* / enemyN_attack_*
    LoadEnemiesFromDisk();
    // 英雄：回读 img/custom 下 heroN_attack_* / heroN_idle_* / heroN_card.png
    LoadHeroFromDisk();
}

// ---- 弹窗交互动作 ---------------------------------------------------
static void RefreshPopup() {
    if (g_designPopup) InvalidateRect(g_designPopup, nullptr, FALSE);
}

// 取当前敌人某一套动画的 路径容器 / 缩略图容器（which: 0=行走 1=攻击）
static std::vector<std::wstring>& EnemyPathsRef(EnemyData& e, int which) {
    return (which == 0) ? e.walkPaths : e.attackPaths;
}
static std::vector<std::unique_ptr<Bitmap>>& EnemyThumbsRef(EnemyData& e, int which) {
    return (which == 0) ? e.walkThumbs : e.attackThumbs;
}
static const wchar_t* EnemyKindName(int which) {
    return (which == 0) ? L"行走动画" : L"攻击动画";
}

// 添加当前敌人的某一套帧图（which: 0=行走 1=攻击）
static void AddEnemyFromDialog(int which) {
    EnemyData& enemy = CurEnemy();
    auto& paths  = EnemyPathsRef(enemy, which);
    auto& thumbs = EnemyThumbsRef(enemy, which);
    const int total  = EnemyFrameTotal(which);
    const int remain = total - (int)paths.size();
    if (remain <= 0) {
        g_popupTip = L"敌人 #" + std::to_wstring(g_enemyIndex + 1) + L" 的「" +
                     std::wstring(EnemyKindName(which)) + L"」已满（上限 " +
                     std::to_wstring(total) + L" 张），请先移除再添加";
        RefreshPopup();
        return;
    }

    const std::wstring title = L"选择敌人 #" + std::to_wstring(g_enemyIndex + 1) + L"「" +
                               std::wstring(EnemyKindName(which)) +
                               L"」图片（一次最多 " + std::to_wstring(remain) +
                               L" 张，共 " + std::to_wstring(total) + L" 帧）";
    std::vector<std::wstring> picked;
    if (!SelectEntityImages(picked, title.c_str(), remain)) return;

    int skipped = 0;
    for (size_t i = 0; i < picked.size(); ++i) {
        if ((int)paths.size() >= total) break;
        std::unique_ptr<Bitmap> thumb = LoadScaledImage(picked[i], 256);
        if (!thumb) { skipped++; continue; }
        paths.push_back(picked[i]);
        thumbs.push_back(std::move(thumb));
    }

    g_popupTip = L"敌人 #" + std::to_wstring(g_enemyIndex + 1) + L"「" +
                 std::wstring(EnemyKindName(which)) + L"」已选 " +
                 std::to_wstring((int)paths.size()) + L" / " +
                 std::to_wstring(total) + L" 张 —— 记得点「保存并应用」";
    if (skipped > 0) g_popupTip += L"｜有 " + std::to_wstring(skipped) + L" 张无法读取，已跳过";
    RefreshPopup();
}

// 移除当前敌人某一套里的第 index 帧
static void RemoveEnemyAt(int which, int index) {
    EnemyData& enemy = CurEnemy();
    auto& paths  = EnemyPathsRef(enemy, which);
    auto& thumbs = EnemyThumbsRef(enemy, which);
    if (index < 0 || index >= (int)paths.size()) return;
    paths.erase(paths.begin() + index);
    thumbs.erase(thumbs.begin() + index);
    g_popupTip = L"已移除 1 张 —— 记得点「保存并应用」";
    RefreshPopup();
}

// ---- 敌方卡面（1 张）-------------------------------------------------
// 槽位放在敌人页签说明面板的右侧（与英雄页的卡面 / 子弹槽同规格）
static RectF EnemyCardRect() {
    return RectF(S(706.f), S(516.f), S(116.f), S(92.f));
}

// 选一张图当这名敌人的卡面（「已有卡牌 → 敌人卡牌」里显示的封面）
static void AddEnemyCardFromDialog() {
    EnemyData& enemy = CurEnemy();
    std::vector<std::wstring> picked;
    const std::wstring title = L"选择敌人 #" + std::to_wstring(g_enemyIndex + 1) +
                               L" 的卡牌图片（1 张；已有卡牌 → 敌人卡牌 的封面）";
    if (!SelectEntityImages(picked, title.c_str(), 1) || picked.empty()) return;
    std::unique_ptr<Bitmap> thumb = LoadScaledImage(picked[0], 256);
    if (!thumb) {
        g_popupTip = L"该图片无法读取，请换一张";
        RefreshPopup();
        return;
    }
    enemy.cardPath  = picked[0];
    enemy.cardThumb = std::move(thumb);
    g_popupTip = L"已选敌人 #" + std::to_wstring(g_enemyIndex + 1) +
                 L" 的卡牌图片 —— 记得点「保存并应用」后生效";
    RefreshPopup();
}

static void RemoveEnemyCard() {
    EnemyData& enemy = CurEnemy();
    enemy.cardPath.clear();
    enemy.cardThumb.reset();
    DeleteFileW(EnemyCardFilePath(g_enemyIndex).c_str());
    g_popupTip = L"已移除敌人卡面 —— 记得点「保存并应用」";
    RefreshPopup();
}

static void ClearCurrentTab() {
    // 英雄页签的数据存在独立容器里，不能当普通分类去取下标（会越界崩溃）
    if (g_bioTab == kHeroTab) {
        HeroData& hero = CurHero();
        hero.attackPaths.clear(); hero.attackThumbs.clear();
        hero.idlePaths.clear();   hero.idleThumbs.clear();
        hero.cardPath.clear();    hero.cardThumb.reset();
        hero.bulletPath.clear();  hero.bulletThumb.reset();
        g_heroHoverSlot = -1; g_heroHoverWhich = -1; g_heroHoverCard = false;
        g_heroHoverBullet = false;
        g_popupTip = L"已清空英雄 #" + std::to_wstring(g_heroIndex + 1) +
                     L"（攻击 8 帧 / 休闲 8 帧 / 卡面），点「保存并应用」后生效";
        RefreshPopup();
        return;
    }
    // 敌人：清空当前编辑的这一名的全部动画帧 + 卡面
    EnemyData& enemy = CurEnemy();
    enemy.walkPaths.clear();   enemy.walkThumbs.clear();
    enemy.attackPaths.clear(); enemy.attackThumbs.clear();
    enemy.cardPath.clear();    enemy.cardThumb.reset();
    g_enemyHoverSlot = -1; g_enemyHoverWhich = -1; g_enemyHoverPick = -1;
    g_enemyHoverCard = false;
    g_popupTip = L"已清空敌人 #" + std::to_wstring(g_enemyIndex + 1) +
                 L"（行走 6 帧 / 攻击 8 帧 / 卡面），点「保存并应用」后生效";
    RefreshPopup();
}

// ---- 英雄：增删 / 清空 / 落盘（路径函数见上方 HeroFilePath / HeroCardFilePath）----

// 英雄的两个单图槽（位于英雄页签信息面板右侧）：卡面在左、子弹在右
static RectF HeroCardRect() {
    return RectF(S(574.f), S(516.f), S(116.f), S(92.f));
}
static RectF HeroBulletRect() {
    return RectF(S(706.f), S(516.f), S(116.f), S(92.f));
}
static RectF HeroCardRemoveRect(const RectF& slot) {
    return RectF(slot.X + slot.Width - S(22.f), slot.Y + S(2.f), S(20.f), S(20.f));
}

// 添加当前英雄的某一套帧图（which: 0=攻击 1=休闲）
static void AddHeroFromDialog(int which) {
    HeroData& hero = CurHero();
    auto& paths  = (which == 0) ? hero.attackPaths  : hero.idlePaths;
    auto& thumbs = (which == 0) ? hero.attackThumbs : hero.idleThumbs;
    const int remain = HERO_FRAMES - (int)paths.size();
    if (remain <= 0) {
        g_popupTip = std::wstring(L"英雄 #") + std::to_wstring(g_heroIndex + 1) + L" 的「" +
                     (which == 0 ? L"攻击状态" : L"休闲状态") + L"」已满（上限 8 张），请先移除再添加";
        RefreshPopup();
        return;
    }
    const std::wstring title = L"选择英雄 #" + std::to_wstring(g_heroIndex + 1) + L"「" +
                               std::wstring(which == 0 ? L"攻击状态" : L"休闲状态") +
                               L"」图片（一次最多 " + std::to_wstring(remain) + L" 张，共 8 帧）";
    std::vector<std::wstring> picked;
    if (!SelectEntityImages(picked, title.c_str(), remain)) return;

    int skipped = 0;
    for (size_t i = 0; i < picked.size(); ++i) {
        if ((int)paths.size() >= HERO_FRAMES) break;
        std::unique_ptr<Bitmap> thumb = LoadScaledImage(picked[i], 256);
        if (!thumb) { skipped++; continue; }
        paths.push_back(picked[i]);
        thumbs.push_back(std::move(thumb));
    }
    g_popupTip = L"英雄 #" + std::to_wstring(g_heroIndex + 1) + L"「" +
                 std::wstring(which == 0 ? L"攻击状态" : L"休闲状态") +
                 L"」已选 " + std::to_wstring((int)paths.size()) +
                 L" / 8 张 —— 记得点「保存并应用」";
    if (skipped > 0) g_popupTip += L"｜有 " + std::to_wstring(skipped) + L" 张无法读取，已跳过";
    RefreshPopup();
}

// 移除当前英雄某一套里的第 index 帧
static void RemoveHeroAt(int which, int index) {
    HeroData& hero = CurHero();
    auto& paths  = (which == 0) ? hero.attackPaths  : hero.idlePaths;
    auto& thumbs = (which == 0) ? hero.attackThumbs : hero.idleThumbs;
    if (index < 0 || index >= (int)paths.size()) return;
    paths.erase(paths.begin() + index);
    thumbs.erase(thumbs.begin() + index);
    g_popupTip = L"已移除 1 张 —— 记得点「保存并应用」";
    RefreshPopup();
}

// 清空当前英雄的某一套图
static void ClearHero(int which) {
    HeroData& hero = CurHero();
    auto& paths  = (which == 0) ? hero.attackPaths  : hero.idlePaths;
    auto& thumbs = (which == 0) ? hero.attackThumbs : hero.idleThumbs;
    paths.clear();
    thumbs.clear();
    g_popupTip = L"已清空英雄 #" + std::to_wstring(g_heroIndex + 1) + L" 的「" +
                 std::wstring(which == 0 ? L"攻击状态" : L"休闲状态") +
                 L"」，点「保存并应用」后生效";
    RefreshPopup();
}

// 打开弹窗时回读全部英雄（每个英雄两套各 8 帧 + 卡面），与"当前已生效"的配置一致
static void LoadHeroFromDisk() {
    for (int h = 0; h < MAX_HEROES; ++h) {
        HeroData& hero = g_heroes[h];
        hero.attackPaths.clear(); hero.attackThumbs.clear();
        hero.idlePaths.clear();   hero.idleThumbs.clear();
        hero.cardPath.clear();    hero.cardThumb.reset();
        hero.bulletPath.clear();  hero.bulletThumb.reset();

        for (int which = 0; which < 2; ++which) {
            auto& paths  = (which == 0) ? hero.attackPaths  : hero.idlePaths;
            auto& thumbs = (which == 0) ? hero.attackThumbs : hero.idleThumbs;
            for (int frame = 1; frame <= HERO_FRAMES; ++frame) {
                std::wstring path;
                if (!HeroFramePath(h, which, frame, path)) break;
                std::unique_ptr<Bitmap> thumb = LoadScaledImage(path, 256);
                if (!thumb) break;
                paths.push_back(path);
                thumbs.push_back(std::move(thumb));
            }
        }
        // 英雄卡面（工具栏图标）：有则载入缩略图
        std::wstring card;
        if (HeroCardPathOnDisk(h, card)) {
            hero.cardPath = card;
            hero.cardThumb = LoadScaledImage(card, 256);
        }
        // 英雄子弹图（远程英雄）：有则载入缩略图
        std::wstring bullet;
        if (HeroBulletPathOnDisk(h, bullet)) {
            hero.bulletPath = bullet;
            hero.bulletThumb = LoadScaledImage(bullet, 256);
        }
    }
}

// 把全部英雄（最多 13 个）的图写入 img/custom（多余帧的旧文件会删除）
static bool ApplyHeroToDisk(int& written, int& failed) {
    CreateDirectoryW(L"img", nullptr);
    CreateDirectoryW(L"img\\custom", nullptr);

    for (int h = 0; h < MAX_HEROES; ++h) {
        HeroData& hero = g_heroes[h];
        for (int which = 0; which < 2; ++which) {
            auto& paths = (which == 0) ? hero.attackPaths : hero.idlePaths;
            for (int i = 0; i < (int)paths.size() && i < HERO_FRAMES; ++i) {
                if (WritePngFromFile(paths[i], HeroFilePath(h, which, i + 1), 160)) written++;
                else failed++;
            }
            // 删除该套多余的旧帧（序号超过当前数量）
            for (int frame = (int)paths.size() + 1; frame <= HERO_FRAMES; ++frame)
                DeleteFileW(HeroFilePath(h, which, frame).c_str());
        }
        // 英雄卡面（工具栏图标）：有则写入，没配就删掉旧文件
        if (!hero.cardPath.empty()) {
            if (WritePngFromFile(hero.cardPath, HeroCardFilePath(h), 160)) written++;
            else failed++;
        } else {
            DeleteFileW(HeroCardFilePath(h).c_str());
        }
        // 英雄子弹图：有则写入，没配就删掉旧文件（英雄随之变回近战）
        if (!hero.bulletPath.empty()) {
            if (WritePngFromFile(hero.bulletPath, HeroBulletFilePath(h), 160)) written++;
            else failed++;
        } else {
            DeleteFileW(HeroBulletFilePath(h).c_str());
        }
    }

    // 旧版单英雄文件名（hero_attack_N / hero_idle_N / hero_card.png）：
    // 1 号英雄已经写成新版了，这里把旧文件清掉，避免旧图"复活"覆盖了删除操作。
    for (int frame = 1; frame <= HERO_FRAMES; ++frame) {
        DeleteFileW(LegacyHeroFilePath(0, frame).c_str());
        DeleteFileW(LegacyHeroFilePath(1, frame).c_str());
    }
    DeleteFileW(LegacyHeroCardPath().c_str());

    return failed == 0 || written > 0;
}

// 英雄卡面（工具栏图标）：单独一张，复用多选对话框（限制选 1 张）
static void AddHeroCardFromDialog() {
    HeroData& hero = CurHero();
    std::vector<std::wstring> picked;
    if (!SelectEntityImages(picked, L"选择英雄卡面图片（工具栏图标，1 张）", 1) || picked.empty())
        return;
    std::unique_ptr<Bitmap> thumb = LoadScaledImage(picked[0], 256);
    if (!thumb) {
        g_popupTip = L"该图片无法读取，请换一张";
        RefreshPopup();
        return;
    }
    hero.cardPath = picked[0];
    hero.cardThumb = std::move(thumb);
    g_popupTip = L"已选英雄 #" + std::to_wstring(g_heroIndex + 1) +
                 L" 的卡面 —— 记得点「保存并应用」后生效";
    RefreshPopup();
}

static void RemoveHeroCard() {
    HeroData& hero = CurHero();
    hero.cardPath.clear();
    hero.cardThumb.reset();
    DeleteFileW(HeroCardFilePath(g_heroIndex).c_str());
    g_popupTip = L"已移除英雄卡面 —— 记得点「保存并应用」";
    RefreshPopup();
}

// 英雄子弹图（1 张）：配了就是远程英雄，没配就是近战英雄
static void AddHeroBulletFromDialog() {
    HeroData& hero = CurHero();
    std::vector<std::wstring> picked;
    if (!SelectEntityImages(picked, L"选择英雄的子弹图片（1 张；配了即为远程攻击）", 1) || picked.empty())
        return;
    std::unique_ptr<Bitmap> thumb = LoadScaledImage(picked[0], 256);
    if (!thumb) {
        g_popupTip = L"该图片无法读取，请换一张";
        RefreshPopup();
        return;
    }
    hero.bulletPath  = picked[0];
    hero.bulletThumb = std::move(thumb);
    g_popupTip = L"已选英雄 #" + std::to_wstring(g_heroIndex + 1) +
                 L" 的子弹图 → 该英雄变为远程攻击 —— 记得点「保存并应用」";
    RefreshPopup();
}

static void RemoveHeroBullet() {
    HeroData& hero = CurHero();
    hero.bulletPath.clear();
    hero.bulletThumb.reset();
    DeleteFileW(HeroBulletFilePath(g_heroIndex).c_str());
    g_popupTip = L"已移除子弹图 → 该英雄恢复近身攻击 —— 记得点「保存并应用」";
    RefreshPopup();
}

const wchar_t* GetImportedEntityPath() {
    return g_importedEntityPath.empty() ? nullptr : g_importedEntityPath.c_str();
}

// ---- 弹窗绘制小件 ---------------------------------------------------
static void DrawPopupButton(Graphics& g, const RectF& r, const std::wstring& label,
                            const Color& fillColor, const Color& textColor) {
    GraphicsPath path;
    AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(12.f));
    SolidBrush fill(fillColor);
    g.FillPath(&fill, &path);
    DrawTextInRect(g, label, r, g_fonts.button.get(), textColor);
}

static void DrawTab(Graphics& g, int index, bool active) {
    const RectF r = TabRect(index);
    GraphicsPath path;
    AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(10.f));
    SolidBrush fill(active ? Color(255, 0x2b, 0x2b, 0x36) : Color(255, 0x1a, 0x1a, 0x21));
    g.FillPath(&fill, &path);
    Pen border(active ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x34, 0x34, 0x3e), g_scale);
    g.DrawPath(&border, &path);

    std::wstring label = kBioInfo[index].name;
    label += L"  " + std::to_wstring(EnemyConfiguredCount()) + L"/" + std::to_wstring(kMaxBioSlots);
    DrawTextInRect(g, label, r, g_fonts.button.get(),
                   active ? Color(255, 0xf2, 0xf2, 0xf5) : Color(255, 0x96, 0x96, 0xa2));
}

// 缩略图槽：有图 → 显示图 + 悬停时右上角移除角标；空 → 虚线框 + ＋
// selected = 这张图正在被编辑数值（描边变绿）
static void DrawBioSlot(Graphics& g, const RectF& slot, Bitmap* image, int number,
                        bool hovered, bool selected = false) {
    GraphicsPath path;
    AddRoundRect(path, slot.X, slot.Y, slot.Width, slot.Height, S(12.f));

    if (image) {
        SolidBrush fill(Color(255, 0x1b, 0x1b, 0x22));
        g.FillPath(&fill, &path);
        const float scale = std::min((slot.Width - S(14.f)) / image->GetWidth(),
                                     (slot.Height - S(14.f)) / image->GetHeight());
        const float drawW = image->GetWidth() * scale, drawH = image->GetHeight() * scale;
        g.DrawImage(image, slot.X + (slot.Width - drawW) * 0.5f,
                    slot.Y + (slot.Height - drawH) * 0.5f, drawW, drawH);
        Pen border(selected ? Color(255, 0x86, 0xd9, 0x8a)
                            : (hovered ? Color(255, 0xe0, 0x8a, 0x8a) : Color(255, 0x33, 0x33, 0x3d)),
                   selected ? g_scale * 2.f : g_scale);
        g.DrawPath(&border, &path);
        if (hovered && !selected) {
            const RectF badge = SlotRemoveRect(slot);
            GraphicsPath badgePath;
            AddRoundRect(badgePath, badge.X, badge.Y, badge.Width, badge.Height, S(8.f));
            SolidBrush badgeFill(Color(235, 0xc0, 0x50, 0x50));
            g.FillPath(&badgeFill, &badgePath);
            DrawTextInRect(g, L"×", badge, g_fonts.status.get(), Color(255, 0xff, 0xff, 0xff));
        }
    } else {
        SolidBrush fill(Color(255, 0x16, 0x16, 0x1c));
        g.FillPath(&fill, &path);
        Pen dash(hovered ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x34, 0x34, 0x3e), g_scale);
        dash.SetDashStyle(DashStyleDash);
        g.DrawPath(&dash, &path);
        DrawTextInRect(g, L"＋", RectF(slot.X, slot.Y + S(8.f), slot.Width, slot.Height - S(24.f)),
                       g_fonts.button.get(),
                       hovered ? Color(255, 0x9e, 0xe8, 0xa2) : Color(255, 0x48, 0x48, 0x54));
        DrawTextInRect(g, std::to_wstring(number),
                       RectF(slot.X + slot.Width - S(26.f), slot.Y + slot.Height - S(26.f), S(20.f), S(18.f)),
                       g_fonts.footer.get(), Color(255, 0x48, 0x48, 0x54));
    }
}

// 英雄卡面槽：已上传则显示缩略图（悬停显示 × 可移除），未上传则显示「＋ 上传卡面」
// 单图槽（卡面 / 子弹共用）：hint 是空槽时的引导文字
static void DrawHeroCardSlot(Graphics& g, const RectF& slot, Bitmap* image, bool hovered,
                             const wchar_t* hint = L"上传卡面") {
    GraphicsPath path;
    AddRoundRect(path, slot.X, slot.Y, slot.Width, slot.Height, S(12.f));
    if (image) {
        SolidBrush fill(Color(255, 0x1b, 0x1b, 0x22));
        g.FillPath(&fill, &path);
        const float scale = std::min((slot.Width - S(14.f)) / image->GetWidth(),
                                     (slot.Height - S(14.f)) / image->GetHeight());
        const float drawW = image->GetWidth() * scale, drawH = image->GetHeight() * scale;
        g.DrawImage(image, slot.X + (slot.Width - drawW) * 0.5f,
                    slot.Y + (slot.Height - drawH) * 0.5f, drawW, drawH);
        Pen border(hovered ? Color(255, 0xe0, 0x8a, 0x8a) : Color(255, 0x33, 0x33, 0x3d), g_scale);
        g.DrawPath(&border, &path);
        if (hovered) {
            const RectF badge = HeroCardRemoveRect(slot);
            GraphicsPath badgePath;
            AddRoundRect(badgePath, badge.X, badge.Y, badge.Width, badge.Height, S(8.f));
            SolidBrush badgeFill(Color(235, 0xc0, 0x50, 0x50));
            g.FillPath(&badgeFill, &badgePath);
            DrawTextInRect(g, L"×", badge, g_fonts.status.get(), Color(255, 0xff, 0xff, 0xff));
        }
    } else {
        SolidBrush fill(Color(255, 0x16, 0x16, 0x1c));
        g.FillPath(&fill, &path);
        Pen dash(hovered ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x34, 0x34, 0x3e), g_scale);
        dash.SetDashStyle(DashStyleDash);
        g.DrawPath(&dash, &path);
        DrawTextInRect(g, L"＋", RectF(slot.X, slot.Y + S(20.f), slot.Width, slot.Height - S(44.f)),
                       g_fonts.button.get(),
                       hovered ? Color(255, 0x9e, 0xe8, 0xa2) : Color(255, 0x48, 0x48, 0x54));
        DrawTextInRect(g, hint, RectF(slot.X, slot.Y + slot.Height - S(34.f), slot.Width, S(24.f)),
                       g_fonts.footer.get(), Color(255, 0x68, 0x68, 0x74));
    }
}

// ---- 弹窗界面绘制（与主界面同风格：深色渐变 / 圆角 / 双缓冲无闪烁） ----
static void DrawDesignPopupScene(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const float W = kPopupW * g_scale, H = kPopupH * g_scale;

    // 背景：与主界面同款垂直渐变
    RectF full(0.f, 0.f, W, H);
    LinearGradientBrush bg(full, Color(255, 0x14, 0x14, 0x18),
                           Color(255, 0x1f, 0x1f, 0x26), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);

    // 标题 + 强调短线（同主界面版式）
    DrawTextInRect(g, L"设计角色", RectF(S(36.f), S(22.f), S(300.f), S(52.f)),
                   g_fonts.title.get(), Color(255, 0xf2, 0xf2, 0xf5), StringAlignmentNear);
    DrawTextInRect(g, L"上传图片即可，无需改动代码；保存后重新开始游戏生效",
                   RectF(S(220.f), S(44.f), W - S(300.f), S(24.f)),
                   g_fonts.footer.get(), Color(255, 0x78, 0x78, 0x82), StringAlignmentFar);
    {
        GraphicsPath bar;
        AddRoundRect(bar, S(36.f), S(78.f), S(48.f), S(3.f), S(1.5f));
        SolidBrush barBr(Color(255, 0x86, 0xd9, 0x8a));
        g.FillPath(&barBr, &bar);
    }

    // 页签（敌人生物）+ 英雄页签
    for (int i = 0; i < kTabCount; ++i) DrawTab(g, i, i == g_bioTab);
    {
        const RectF r = TabRect(kHeroTab);
        GraphicsPath path;
        AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(10.f));
        SolidBrush fill(g_bioTab == kHeroTab ? Color(255, 0x2b, 0x2b, 0x36) : Color(255, 0x1a, 0x1a, 0x21));
        g.FillPath(&fill, &path);
        Pen border(g_bioTab == kHeroTab ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x34, 0x34, 0x3e), g_scale);
        g.DrawPath(&border, &path);
        DrawTextInRect(g, L"英雄  " + std::to_wstring(HeroConfiguredCount()) + L"/" +
                          std::to_wstring(MAX_HEROES),
                       r, g_fonts.button.get(),
                       g_bioTab == kHeroTab ? Color(255, 0xf2, 0xf2, 0xf5) : Color(255, 0x96, 0x96, 0xa2));
    }

    if (g_bioTab == kHeroTab)
        DrawTextInRect(g, L"最多可添加 13 个英雄：点编号切换当前英雄，各传「攻击状态」「休闲状态」8 张图，卡面与子弹图可选（传子弹 = 远程）",
                       RectF(S(36.f), S(158.f), W - S(72.f), S(24.f)),
                       g_fonts.status.get(), Color(255, 0x9a, 0x9a, 0xa6), StringAlignmentNear);
    else
        DrawTextInRect(g, kBioInfo[g_bioTab].hint,
                       RectF(S(36.f), S(158.f), W - S(72.f), S(24.f)),
                       g_fonts.status.get(), Color(255, 0x9a, 0x9a, 0xa6), StringAlignmentNear);

    if (g_bioTab == kHeroTab) {
        // 英雄编号按钮（1..13）：当前编辑的那个高亮，已配图的编号带绿边
        for (int i = 0; i < MAX_HEROES; ++i) {
            const RectF r = HeroPickRect(i);
            const bool active = (i == g_heroIndex);
            const bool hot    = (g_heroHoverPick == i);
            GraphicsPath path;
            AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(8.f));
            SolidBrush fill(active ? Color(255, 0x1d, 0x2a, 0x1d)
                                   : (hot ? Color(255, 0x24, 0x24, 0x2c) : Color(255, 0x1a, 0x1a, 0x21)));
            g.FillPath(&fill, &path);
            Pen border(active ? Color(255, 0x86, 0xd9, 0x8a)
                              : (HeroConfigured(i) ? Color(255, 0x4a, 0x6a, 0x4a)
                                                   : Color(255, 0x34, 0x34, 0x3e)), g_scale);
            g.DrawPath(&border, &path);
            DrawTextInRect(g, std::to_wstring(i + 1), r, g_fonts.button.get(),
                           active ? Color(255, 0xf2, 0xf2, 0xf5)
                                  : (HeroConfigured(i) ? Color(255, 0x9e, 0xe8, 0xa2)
                                                       : Color(255, 0x78, 0x78, 0x82)));
        }

        // 当前英雄：两套各 8 帧图网格（攻击在上、休闲在下）
        const HeroData& hero = CurHero();
        for (int which = 0; which < 2; ++which) {
            const auto& thumbs = (which == 0) ? hero.attackThumbs : hero.idleThumbs;
            std::wstring label = std::wstring(L"英雄 #") + std::to_wstring(g_heroIndex + 1) +
                                 (which == 0 ? L" 攻击状态（8 帧）" : L" 休闲状态（8 帧）");
            if (which == 0)   // 攻击行标出这个英雄是远程还是近身
                label += HeroIsRanged(g_heroIndex) ? L"　·　远程" : L"　·　近身";
            DrawTextInRect(g, label, HeroGridLabelRect(which), g_fonts.status.get(),
                           Color(255, 0xb8, 0xb8, 0xc2), StringAlignmentNear);
            for (int i = 0; i < HERO_FRAMES; ++i) {
                Bitmap* thumb = (i < (int)thumbs.size()) ? thumbs[i].get() : nullptr;
                const bool hovered = (g_heroHoverWhich == which && g_heroHoverSlot == i);
                DrawBioSlot(g, HeroSlotRect(which, i), thumb, i + 1, hovered, false);
            }
        }
    } else {
        // 敌人编号按钮（1..13）：当前编辑的那个高亮，已配图的编号带绿边
        for (int i = 0; i < kMaxBioSlots; ++i) {
            const RectF r = EnemyPickRect(i);
            const bool active = (i == g_enemyIndex);
            const bool hot    = (g_enemyHoverPick == i);
            GraphicsPath path;
            AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(8.f));
            SolidBrush fill(active ? Color(255, 0x1d, 0x2a, 0x1d)
                                   : (hot ? Color(255, 0x24, 0x24, 0x2c) : Color(255, 0x1a, 0x1a, 0x21)));
            g.FillPath(&fill, &path);
            Pen border(active ? Color(255, 0x86, 0xd9, 0x8a)
                              : (EnemyConfigured(i) ? Color(255, 0x4a, 0x6a, 0x4a)
                                                     : Color(255, 0x34, 0x34, 0x3e)), g_scale);
            g.DrawPath(&border, &path);
            DrawTextInRect(g, std::to_wstring(i + 1), r, g_fonts.button.get(),
                           active ? Color(255, 0xf2, 0xf2, 0xf5)
                                  : (EnemyConfigured(i) ? Color(255, 0x9e, 0xe8, 0xa2)
                                                         : Color(255, 0x78, 0x78, 0x82)));
        }

        // 当前敌人：两套动画帧网格（行走 6 帧在上、攻击 8 帧在下）
        EnemyData& enemy = CurEnemy();
        for (int which = 0; which < 2; ++which) {
            const auto& thumbs = EnemyThumbsRef(enemy, which);
            std::wstring label = std::wstring(L"敌人 #") + std::to_wstring(g_enemyIndex + 1) +
                                 (which == 0 ? L" 行走动画（6 帧）" : L" 攻击动画（8 帧）");
            DrawTextInRect(g, label, EnemyGridLabelRect(which), g_fonts.status.get(),
                           Color(255, 0xb8, 0xb8, 0xc2), StringAlignmentNear);
            for (int i = 0; i < EnemyFrameTotal(which); ++i) {
                Bitmap* thumb = (i < (int)thumbs.size()) ? thumbs[i].get() : nullptr;
                const bool hovered = (g_enemyHoverWhich == which && g_enemyHoverSlot == i);
                DrawBioSlot(g, EnemySlotRect(which, i), thumb, i + 1, hovered, false);
            }
        }
    }

    // ---- 数值编辑区：血量 / 费用 / 伤害 ----
    // 敌人页按编号编辑、英雄页按英雄编号编辑 → 两者都始终有编辑目标
    const bool hasSel = true;
    DrawTextInRect(g, g_bioTab == kHeroTab
                        ? (L"英雄 #" + std::to_wstring(g_heroIndex + 1) +
                           L" 的数值（每个英雄各一套，直接点 ＋ / − 修改）")
                        : (L"敌人 #" + std::to_wstring(g_enemyIndex + 1) +
                           L" 的数值（每个敌人各一套：血量 + 伤害，无费用）"),
                   RectF(S(64.f), S(414.f), W - S(128.f), S(24.f)),
                   g_fonts.status.get(),
                   Color(255, 0x9e, 0xe8, 0xa2),
                   StringAlignmentNear);

    for (int w = 0; w < STAT_COUNT; ++w) {
        const bool applicable = (g_bioTab == kHeroTab) ? true : BioHasStat(g_bioTab, w);   // 英雄三项全开；其余分类按规则
        const bool editable   = applicable && hasSel;
        const RectF box = StatGroupRect(w);

        GraphicsPath path;
        AddRoundRect(path, box.X, box.Y, box.Width, box.Height, S(10.f));
        SolidBrush fill(editable ? Color(255, 0x1d, 0x1d, 0x25) : Color(255, 0x15, 0x15, 0x19));
        g.FillPath(&fill, &path);
        Pen border(editable ? Color(255, 0x3c, 0x3c, 0x48) : Color(255, 0x24, 0x24, 0x2a), g_scale);
        g.DrawPath(&border, &path);

        DrawTextInRect(g, kStatNames[w], RectF(box.X + S(12.f), box.Y, S(56.f), box.Height),
                       g_fonts.status.get(),
                       editable ? Color(255, 0xc8, 0xc8, 0xd2) : Color(255, 0x4e, 0x4e, 0x58),
                       StringAlignmentNear);
        if (!applicable) continue;      // 这类生物没有这项数值 → 只留一个灰掉的框

        // − 值 +
        for (int s = 0; s < 2; ++s) {
            const RectF btn = (s == 0) ? StatMinusRect(w) : StatPlusRect(w);
            const bool hot  = editable && (g_hoverStatBtn == w * 2 + s);
            GraphicsPath bp;
            AddRoundRect(bp, btn.X, btn.Y, btn.Width, btn.Height, S(8.f));
            SolidBrush bf(hot ? Color(255, 0x86, 0xd9, 0x8a)
                              : (editable ? Color(255, 0x2a, 0x2a, 0x34) : Color(255, 0x1e, 0x1e, 0x24)));
            g.FillPath(&bf, &bp);
            DrawTextInRect(g, (s == 0) ? L"−" : L"＋", btn, g_fonts.button.get(),
                           hot ? Color(255, 0x12, 0x1a, 0x16)
                               : (editable ? Color(255, 0xd8, 0xd8, 0xe0) : Color(255, 0x4a, 0x4a, 0x54)));
        }
        int* field = nullptr;
        if (g_bioTab == kHeroTab) {                          // 英雄：指向当前英雄自己的数值
            BioStat& hs = g_heroStat[g_heroIndex];
            field = (w == STAT_HP)   ? &hs.hp
                  : (w == STAT_COST) ? &hs.cost
                                     : &hs.dmg;
        } else {                                            // 敌人：当前编辑的敌人编号
            field = BioStatField(BIO_ENEMY, g_enemyIndex, w);
        }
        DrawTextInRect(g, field ? std::to_wstring(*field) : L"—",
                       StatValueRect(w), g_fonts.button.get(),
                       hasSel ? Color(255, 0xff, 0xec, 0x96) : Color(255, 0x4e, 0x4e, 0x58));
    }

    // ---- 说明面板 ----
    if (g_bioTab == kHeroTab) {
        RectF area(S(64.f), S(502.f), W - S(128.f), S(118.f));
        GraphicsPath path;
        AddRoundRect(path, area.X, area.Y, area.Width, area.Height, S(12.f));
        SolidBrush fill(Color(255, 0x16, 0x16, 0x1c));
        g.FillPath(&fill, &path);
        Pen border(Color(60, 0x55, 0x55, 0x60), g_scale);
        g.DrawPath(&border, &path);
        // 右侧两个单图槽：卡面（工具栏图标） + 子弹图（配了 = 远程英雄）
        const RectF cardR = HeroCardRect();
        DrawHeroCardSlot(g, cardR, CurHero().cardThumb.get(), g_heroHoverCard, L"上传卡面");
        DrawTextInRect(g, L"英雄卡面（工具栏图标）",
                       RectF(cardR.X, area.Y + S(2.f), cardR.Width, S(16.f)),
                       g_fonts.footer.get(), Color(255, 0x8a, 0x8a, 0x96), StringAlignmentNear);

        const RectF bulletR = HeroBulletRect();
        DrawHeroCardSlot(g, bulletR, CurHero().bulletThumb.get(), g_heroHoverBullet, L"上传子弹");
        DrawTextInRect(g, CurHero().bulletPath.empty() ? L"子弹图（不传 = 近身攻击）"
                                                      : L"子弹图（已配 = 远程攻击）",
                       RectF(bulletR.X, area.Y + S(2.f), bulletR.Width, S(16.f)),
                       g_fonts.footer.get(),
                       CurHero().bulletPath.empty() ? Color(255, 0x8a, 0x8a, 0x96)
                                                    : Color(255, 0x9e, 0xe8, 0xa2),
                       StringAlignmentNear);
        // 左侧说明文字
        DrawTextInRect(g, L"每个英雄都能单独上场：可移动位置，同一种英雄场上只能放一个。\n"
                          L"请在上方点编号切换英雄，再分别上传「攻击状态」与「休闲状态」各 8 张图（两段 8 帧动画）。\n"
                          L"下方「数值」是这一个英雄自己的：血量=扛多少伤害，费用=放下要花的阳光，伤害=每发/每刀伤害。\n"
                          L"右侧「英雄卡面」是工具栏图标（可选）；「子弹图」传了就是远程英雄，不传就是近身英雄。",
                       RectF(area.X + S(20.f), area.Y + S(8.f), cardR.X - area.X - S(34.f), area.Height - S(16.f)),
                       g_fonts.status.get(), Color(255, 0x8a, 0x8a, 0x96), StringAlignmentNear);
    } else {
        RectF area(S(64.f), S(502.f), W - S(128.f), S(118.f));
        GraphicsPath path;
        AddRoundRect(path, area.X, area.Y, area.Width, area.Height, S(12.f));
        SolidBrush fill(Color(255, 0x16, 0x16, 0x1c));
        g.FillPath(&fill, &path);
        Pen border(Color(60, 0x55, 0x55, 0x60), g_scale);
        g.DrawPath(&border, &path);
        // 右侧单图槽：敌方卡面（1 张，可选）——「已有卡牌 → 敌人卡牌」的封面
        const RectF enemyCardR = EnemyCardRect();
        DrawHeroCardSlot(g, enemyCardR, CurEnemy().cardThumb.get(), g_enemyHoverCard, L"上传卡面");
        DrawTextInRect(g, L"敌方卡牌图片（1 张）",
                       RectF(enemyCardR.X, area.Y + S(2.f), enemyCardR.Width, S(16.f)),
                       g_fonts.footer.get(), Color(255, 0x8a, 0x8a, 0x96), StringAlignmentNear);
        // 左侧说明文字
        DrawTextInRect(g, L"最多可添加 13 名敌人：先点上方编号切换要编辑的敌人，再上传「行走动画」6 张与「攻击动画」8 张。\n"
                          L"行走帧用于敌人从右侧走过来，攻击帧用于它啃食植物时播放；两段都传满才是完整动画，\n"
                          L"只传其中一段也能正常显示（缺失的那段会退回另一段）。编号可以跳着配，空号会被跳过。\n"
                          L"右侧「敌方卡牌图片」是它在「已有卡牌 → 敌人卡牌」里的封面（可选，不传就显示行走第 1 帧）。",
                       RectF(area.X + S(20.f), area.Y + S(8.f),
                             enemyCardR.X - area.X - S(34.f), area.Height - S(16.f)),
                       g_fonts.status.get(), Color(255, 0x8a, 0x8a, 0x96), StringAlignmentNear);
    }

    // 底部状态提示 + 三个操作按钮
    DrawTextInRect(g, g_popupTip.empty()
                            ? L"提示：点空格子添加图片；鼠标移到图片上，点右上角 × 可移除；改完记得点「保存并应用」"
                            : g_popupTip,
                   TipTextRect(), g_fonts.footer.get(), Color(255, 0x8a, 0x92, 0x9e),
                   StringAlignmentNear);

    DrawPopupButton(g, CloseButtonRect(), L"返回",
                    Color(255, 0x24, 0x24, 0x2c), Color(255, 0xb8, 0xb8, 0xc2));
    DrawPopupButton(g, ClearButtonRect(), L"清空当前角色",
                    Color(255, 0x2a, 0x22, 0x26), Color(255, 0xd8, 0xa0, 0xa0));
    DrawPopupButton(g, ApplyButtonRect(), L"保存并应用",
                    Color(255, 0x86, 0xd9, 0x8a), Color(255, 0x12, 0x1a, 0x16));
}

// ---- 弹窗命中测试 ---------------------------------------------------
enum PopupTarget {
    POPUP_NONE = 0, POPUP_CLOSE, POPUP_CLEAR, POPUP_APPLY,
    POPUP_TAB, POPUP_STAT,
    POPUP_ENEMY_SLOT, POPUP_ENEMY_PICK, POPUP_ENEMY_CARD,
    POPUP_HERO_SLOT, POPUP_HERO_CARD, POPUP_HERO_PICK,
    POPUP_HERO_BULLET
};

static PopupTarget HitTestPopup(const PointF& p, int& index, bool& onRemoveBadge) {
    index = -1;
    onRemoveBadge = false;
    if (CloseButtonRect().Contains(p)) return POPUP_CLOSE;
    if (ClearButtonRect().Contains(p)) return POPUP_CLEAR;
    if (ApplyButtonRect().Contains(p)) return POPUP_APPLY;
    for (int i = 0; i < kTabCount; ++i)
        if (TabRect(i).Contains(p)) { index = i; return POPUP_TAB; }
    if (TabRect(kHeroTab).Contains(p)) { index = kHeroTab; return POPUP_TAB; }
    // 数值按钮（− / ＋）：index = which*2 + (0=−, 1=+)
    for (int w = 0; w < STAT_COUNT; ++w) {
        // 英雄三项数值全开；其余分类按各自规则（与绘制处的 applicable 保持一致）
        const bool applicable = (g_bioTab == kHeroTab) ? true : BioHasStat(g_bioTab, w);
        if (!applicable) continue;
        if (StatMinusRect(w).Contains(p)) { index = w * 2;     return POPUP_STAT; }
        if (StatPlusRect(w).Contains(p))  { index = w * 2 + 1; return POPUP_STAT; }
    }
    // 英雄页签：编号按钮 + 两套各 8 帧图格（攻击在上、休闲在下）；index = which*100 + 帧序号
    if (g_bioTab == kHeroTab) {
        for (int i = 0; i < MAX_HEROES; ++i)
            if (HeroPickRect(i).Contains(p)) { index = i; return POPUP_HERO_PICK; }
        // 英雄卡面槽（工具栏图标）优先判定
        const RectF cardR = HeroCardRect();
        if (cardR.Contains(p)) {
            onRemoveBadge = !CurHero().cardPath.empty() && HeroCardRemoveRect(cardR).Contains(p);
            return POPUP_HERO_CARD;
        }
        // 英雄子弹槽（配了 = 远程英雄）
        const RectF bulletR = HeroBulletRect();
        if (bulletR.Contains(p)) {
            onRemoveBadge = !CurHero().bulletPath.empty() && HeroCardRemoveRect(bulletR).Contains(p);
            return POPUP_HERO_BULLET;
        }
        for (int which = 0; which < 2; ++which) {
            const auto& paths = (which == 0) ? CurHero().attackPaths : CurHero().idlePaths;
            for (int i = 0; i < HERO_FRAMES; ++i) {
                const RectF slot = HeroSlotRect(which, i);
                if (!slot.Contains(p)) continue;
                index = which * 100 + i;
                onRemoveBadge = (i < (int)paths.size()) && HeroSlotRemoveRect(slot).Contains(p);
                return POPUP_HERO_SLOT;
            }
        }
        return POPUP_NONE;
    }
    // 敌人页签：编号按钮 + 两套动画帧网格（行走 6 帧在上、攻击 8 帧在下）
    // index = which*100 + 帧序号
    for (int i = 0; i < kMaxBioSlots; ++i)
        if (EnemyPickRect(i).Contains(p)) { index = i; return POPUP_ENEMY_PICK; }
    // 敌方卡面槽（1 张，可选）优先判定
    const RectF enemyCardR = EnemyCardRect();
    if (enemyCardR.Contains(p)) {
        onRemoveBadge = !CurEnemy().cardPath.empty() && HeroCardRemoveRect(enemyCardR).Contains(p);
        return POPUP_ENEMY_CARD;
    }
    for (int which = 0; which < 2; ++which) {
        const auto& paths = EnemyPathsRef(CurEnemy(), which);
        for (int i = 0; i < EnemyFrameTotal(which); ++i) {
            const RectF slot = EnemySlotRect(which, i);
            if (!slot.Contains(p)) continue;
            index = which * 100 + i;
            onRemoveBadge = (i < (int)paths.size()) && HeroSlotRemoveRect(slot).Contains(p);
            return POPUP_ENEMY_SLOT;
        }
    }
    return POPUP_NONE;
}

// ---- 弹窗消息处理 ----
static LRESULT CALLBACK DesignPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // 双缓冲（与主界面同一套写法，防闪烁、防空句柄）
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem) { EndPaint(hwnd, &ps); return 0; }
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        if (!bmp) { DeleteDC(mem); EndPaint(hwnd, &ps); return 0; }
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        { Graphics gg(mem); DrawDesignPopupScene(gg); }
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old) SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                                  // 背景在 WM_PAINT 里画
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&track);
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1;
        bool onRemove = false;
        const PopupTarget target = HitTestPopup(point, index, onRemove);
        const int hoverStat = (target == POPUP_STAT) ? index : -1;
        int enemyHoverSlot = -1, enemyHoverWhich = -1, enemyHoverPick = -1;
        if (target == POPUP_ENEMY_SLOT) { enemyHoverWhich = index / 100; enemyHoverSlot = index % 100; }
        if (target == POPUP_ENEMY_PICK) enemyHoverPick = index;
        int heroHoverSlot = -1, heroHoverWhich = -1, heroHoverPick = -1;
        if (target == POPUP_HERO_SLOT) { heroHoverWhich = index / 100; heroHoverSlot = index % 100; }
        if (target == POPUP_HERO_PICK) heroHoverPick = index;
        const bool hoverCard       = (target == POPUP_HERO_CARD);
        const bool hoverBullet     = (target == POPUP_HERO_BULLET);
        const bool hoverEnemyCard  = (target == POPUP_ENEMY_CARD);
        if (hoverStat != g_hoverStatBtn ||
            enemyHoverSlot != g_enemyHoverSlot || enemyHoverWhich != g_enemyHoverWhich ||
            enemyHoverPick != g_enemyHoverPick || hoverEnemyCard != g_enemyHoverCard ||
            heroHoverSlot != g_heroHoverSlot || heroHoverWhich != g_heroHoverWhich ||
            heroHoverPick != g_heroHoverPick ||
            hoverCard != g_heroHoverCard || hoverBullet != g_heroHoverBullet) {
            g_hoverStatBtn = hoverStat;
            g_enemyHoverSlot = enemyHoverSlot;
            g_enemyHoverWhich = enemyHoverWhich;
            g_enemyHoverPick = enemyHoverPick;
            g_enemyHoverCard = hoverEnemyCard;
            g_heroHoverSlot = heroHoverSlot;
            g_heroHoverWhich = heroHoverWhich;
            g_heroHoverPick = heroHoverPick;
            g_heroHoverCard = hoverCard;
            g_heroHoverBullet = hoverBullet;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverStatBtn != -1 ||
            g_enemyHoverSlot != -1 || g_enemyHoverWhich != -1 || g_enemyHoverPick != -1 ||
            g_enemyHoverCard ||
            g_heroHoverSlot != -1 || g_heroHoverWhich != -1 || g_heroHoverPick != -1 ||
            g_heroHoverCard || g_heroHoverBullet) {
            g_hoverStatBtn = -1;
            g_enemyHoverSlot = -1;
            g_enemyHoverWhich = -1;
            g_enemyHoverPick = -1;
            g_enemyHoverCard = false;
            g_heroHoverSlot = -1;
            g_heroHoverWhich = -1;
            g_heroHoverPick = -1;
            g_heroHoverCard = false;
            g_heroHoverBullet = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1;
        bool onRemove = false;
        switch (HitTestPopup(point, index, onRemove)) {
        case POPUP_CLOSE:
            DestroyWindow(hwnd);
            break;
        case POPUP_CLEAR:
            ClearCurrentTab();
            break;
        case POPUP_APPLY: {
            std::wstring tip;
            ApplyBiosToDisk(tip);
            g_popupTip = tip;
            RefreshPopup();
            break;
        }
        case POPUP_TAB:
            if (index != g_bioTab) {
                g_bioTab = index;
                g_hoverStatBtn = -1;
                g_enemyHoverSlot = -1;
                g_enemyHoverWhich = -1;
                g_enemyHoverPick = -1;
                g_enemyHoverCard = false;
                g_heroHoverSlot = -1;
                g_heroHoverWhich = -1;
                g_heroHoverPick = -1;
                g_heroHoverCard = false;
                g_heroHoverBullet = false;
                g_popupTip.clear();
                RefreshPopup();
            }
            break;
        case POPUP_HERO_PICK:                               // 切换当前编辑的英雄
            if (index >= 0 && index < MAX_HEROES && index != g_heroIndex) {
                g_heroIndex = index;
                g_heroHoverSlot = -1;
                g_heroHoverWhich = -1;
                g_heroHoverCard = false;
                g_heroHoverBullet = false;
                g_popupTip = L"当前编辑：英雄 #" + std::to_wstring(g_heroIndex + 1);
                RefreshPopup();
            }
            break;
        case POPUP_ENEMY_PICK:                              // 切换当前编辑的敌人
            if (index >= 0 && index < kMaxBioSlots && index != g_enemyIndex) {
                g_enemyIndex = index;
                g_enemyHoverSlot = -1;
                g_enemyHoverWhich = -1;
                g_enemyHoverCard = false;
                g_popupTip = L"当前编辑：敌人 #" + std::to_wstring(g_enemyIndex + 1);
                RefreshPopup();
            }
            break;
        case POPUP_STAT: {                                 // 数值 +/−
            const int which = index / 2;
            const int delta = (index % 2 == 0) ? -1 : 1;
            int* field = nullptr;
            if (g_bioTab == kHeroTab) {                     // 英雄：改当前英雄自己的数值
                BioStat& hs = g_heroStat[g_heroIndex];
                field = (which == STAT_HP)   ? &hs.hp
                      : (which == STAT_COST) ? &hs.cost
                                             : &hs.dmg;
            } else {                                        // 敌人：改当前敌人编号对应的数值
                field = BioStatField(BIO_ENEMY, g_enemyIndex, which);
            }
            if (field == nullptr) {
                g_popupTip = L"这一项数值对当前分类不可用";
            } else {
                *field = std::clamp(*field + delta * kStatStep[which],
                                    kStatMin[which], kStatMax[which]);
                if (g_bioTab == kHeroTab) {
                    g_popupTip = L"已把「英雄 #" + std::to_wstring(g_heroIndex + 1) +
                                 L"」的" + kStatNames[which] +
                                 L"改为 " + std::to_wstring(*field) + L" —— 记得点「保存并应用」";
                } else {
                    g_popupTip = std::wstring(L"已把「敌人 #") +
                                 std::to_wstring(g_enemyIndex + 1) + L"」的" +
                                 kStatNames[which] + L"改为 " + std::to_wstring(*field) +
                                 L" —— 记得点「保存并应用」";
                }
            }
            RefreshPopup();
            break;
        }
        case POPUP_ENEMY_SLOT: {                            // 敌人图格：添加 / 移除
            const int which = index / 100;
            const int i = index % 100;
            auto& paths = EnemyPathsRef(CurEnemy(), which);
            if (i >= (int)paths.size()) {
                AddEnemyFromDialog(which);                 // 空格子 → 添加
            } else if (onRemove) {
                RemoveEnemyAt(which, i);                   // 点右上角 × → 移除
            } else {
                g_popupTip = std::wstring(EnemyKindName(which)) + L" #" +
                             std::to_wstring(i + 1) +
                             L"：要移除请点它右上角的 ×；行走传满 6 张、攻击传满 8 张才是完整动画";
                RefreshPopup();
            }
            break;
        }
        case POPUP_ENEMY_CARD: {                            // 敌方卡面：上传 / 移除
            if (CurEnemy().cardPath.empty()) {
                AddEnemyCardFromDialog();                   // 未上传 → 选一张
            } else if (onRemove) {
                RemoveEnemyCard();                          // 点 × → 移除
            } else {
                g_popupTip = L"已上传敌方卡牌图片；要替换请先点 × 移除再重选，"
                             L"或点「保存并应用」后生效";
                RefreshPopup();
            }
            break;
        }
        case POPUP_HERO_SLOT: {                             // 英雄图格：添加 / 移除
            const int which = index / 100;
            const int i = index % 100;
            auto& paths = (which == 0) ? CurHero().attackPaths : CurHero().idlePaths;
            if (i >= (int)paths.size()) {
                AddHeroFromDialog(which);                  // 空格子 → 添加
            } else if (onRemove) {
                RemoveHeroAt(which, i);                    // 点右上角 × → 移除
            } else {
                g_popupTip = std::wstring(which == 0 ? L"攻击状态" : L"休闲状态") + L" #" +
                             std::to_wstring(i + 1) +
                             L"：要移除请点它右上角的 ×；两套各传 8 张才会是完整动画";
                RefreshPopup();
            }
            break;
        }
        case POPUP_HERO_CARD: {                            // 英雄卡面：上传 / 移除
            if (CurHero().cardPath.empty()) {
                AddHeroCardFromDialog();                   // 未上传 → 选一张
            } else if (onRemove) {
                RemoveHeroCard();                          // 点 × → 移除
            } else {
                g_popupTip = L"已上传英雄卡面；要替换请先点 × 移除再重选，或点「保存并应用」后生效";
                RefreshPopup();
            }
            break;
        }
        case POPUP_HERO_BULLET: {                          // 英雄子弹图：上传 / 移除
            if (CurHero().bulletPath.empty()) {
                AddHeroBulletFromDialog();                 // 未上传 → 选一张（变远程）
            } else if (onRemove) {
                RemoveHeroBullet();                        // 点 × → 移除（变回近战）
            } else {
                g_popupTip = L"已上传子弹图，该英雄为远程攻击；要替换请先点 × 移除再重选";
                RefreshPopup();
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const PointF clientPoint((REAL)point.x, (REAL)point.y);
            int index = -1;
            bool onRemove = false;
            const PopupTarget target = HitTestPopup(clientPoint, index, onRemove);
            SetCursor(LoadCursor(nullptr, target != POPUP_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);   // Esc 关闭弹窗
        return 0;
    case WM_DESTROY:
        if (g_designPopup == hwnd) g_designPopup = nullptr;
        // 弹窗关闭 → 恢复上层窗口：从子菜单打开则回子菜单，否则回启动器
        if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowCharMenuWindow();
        else ShowLauncherWindow();
        return 0;   // 注意：不调用 PostQuitMessage —— 进程退出只由主窗口决定
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 打开弹窗（已打开则直接置前，不会叠出多个） ----
static void OpenDesignCharacterPopup() {
    if (g_designPopup && IsWindow(g_designPopup)) {
        ShowWindow(g_designPopup, SW_RESTORE);
        SetForegroundWindow(g_designPopup);
        return;
    }

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DesignPopupWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;                 // 自绘背景
        wc.lpszClassName = kPopupClass;
        RegisterClassExW(&wc);
        s_registered = true;
    }

    // 每次打开都回读 img/custom，界面与"当前已生效"的配置保持一致
    LoadBiosFromDisk();
    g_hoverStatBtn = -1;
    g_enemyHoverSlot = -1;
    g_enemyHoverWhich = -1;
    g_enemyHoverPick = -1;
    g_heroHoverSlot = -1;
    g_heroHoverWhich = -1;
    g_heroHoverPick = -1;
    g_heroHoverCard = false;
    g_heroHoverBullet = false;
    g_popupTip.clear();

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;   // 弹窗：固定尺寸
    RECT rc{ 0, 0, (LONG)(kPopupW * g_scale), (LONG)(kPopupH * g_scale) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    // 在上层窗口正上方居中（优先子菜单，其次启动器，都没有则按屏幕工作区）
    int wx, wy;
    HWND centerOn = (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ? g_charMenuHwnd
                  : (g_mainHwnd && IsWindow(g_mainHwnd))         ? g_mainHwnd
                  : nullptr;
    if (centerOn) {
        RECT mw; GetWindowRect(centerOn, &mw);
        wx = mw.left + ((mw.right - mw.left) - ww) / 2;
        wy = mw.top  + ((mw.bottom - mw.top) - wh) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        wx = wa.left + ((wa.right - wa.left) - ww) / 2;
        wy = wa.top  + ((wa.bottom - wa.top) - wh) / 2;
    }

    g_designPopup = CreateWindowExW(0, kPopupClass, L"设计角色",
                                    style, wx, wy, ww, wh,
                                    nullptr,                      // owner 置空：弹窗独立显示
                                    nullptr,                      // 菜单：无
                                    GetModuleHandleW(nullptr), nullptr);

    // 进入弹窗即隐藏当前上层窗口（菜单栏消失），关闭弹窗时再恢复（见 WM_DESTROY）
    if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowWindow(g_charMenuHwnd, SW_HIDE);
    else if (g_mainHwnd) ShowWindow(g_mainHwnd, SW_HIDE);

    if (g_designPopup) {
        ShowWindow(g_designPopup, SW_SHOW);
        UpdateWindow(g_designPopup);
    }
}



// =====================================================================
// 出战卡组弹窗：从已有卡牌中勾选最多 8 张带入游戏 + 是否携带英雄
// ---------------------------------------------------------------------
// deck.txt 约定（img\custom\deck.txt，纯 ASCII）：
//   hero=1            （0/1，是否携带英雄）
//   type=100          （每张被选中的卡一行，值为其 type）
// 没有 deck.txt → g_deckCount=0，game 里回退为「全部可用」。
// =====================================================================
static const wchar_t* kDeckClass  = L"PvZDeckWnd";
static constexpr float kDeckW     = 900.f;   // 与 kPopupW 一致，复用 Close/Apply 按钮版式
static constexpr float kDeckH     = 720.f;
static constexpr int   kDeckCols  = 5;
static constexpr float kDeckTileW = 150.f;
static constexpr float kDeckTileH = 60.f;
static constexpr float kDeckTileGap = 10.f;
static constexpr float kDeckY0    = 132.f;   // 网格起始 y（逻辑）
static constexpr float kDeckHeroY = 552.f;   // 「携带英雄」开关 y（逻辑，固定避免压到网格）

static RectF DeckTileRect(int k) {
    const int col = k % kDeckCols;
    const int row = k / kDeckCols;
    const float gridW = kDeckCols * kDeckTileW + (kDeckCols - 1) * kDeckTileGap;
    const float x0 = (kDeckW - gridW) * 0.5f;
    return RectF(S(x0 + col * (kDeckTileW + kDeckTileGap)),
                 S(kDeckY0 + row * (kDeckTileH + kDeckTileGap)),
                 S(kDeckTileW), S(kDeckTileH));
}
static RectF DeckCheckRect(const RectF& tile) {
    return RectF(tile.X + S(12.f), tile.Y + (tile.Height - S(26.f)) * 0.5f, S(26.f), S(26.f));
}
static RectF DeckHeroRect() {
    const float gridW = kDeckCols * kDeckTileW + (kDeckCols - 1) * kDeckTileGap;
    const float x0 = (kDeckW - gridW) * 0.5f;
    return RectF(S(x0), S(kDeckHeroY), S(gridW), S(48.f));
}

static int DeckPickedCount() {
    int n = 0;
    for (int k = 0; k < g_deckCardCount; ++k) if (g_deckPicked[k]) n++;
    return n;
}

static void DrawDeckScene(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const float W = kDeckW * g_scale, H = kDeckH * g_scale;
    RectF full(0, 0, W, H);
    LinearGradientBrush bg(full, Color(255, 0x14, 0x14, 0x18),
                           Color(255, 0x1f, 0x1f, 0x26), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);

    DrawTextInRect(g, L"出战卡组", RectF(S(36.f), S(22.f), S(300.f), S(52.f)),
                   g_fonts.title.get(), Color(255, 0xf2, 0xf2, 0xf5), StringAlignmentNear);
    {
        GraphicsPath bar;
        AddRoundRect(bar, S(36.f), S(78.f), S(48.f), S(3.f), S(1.5f));
        SolidBrush barBr(Color(255, 0x86, 0xd9, 0x8a));
        g.FillPath(&barBr, &bar);
    }
    DrawTextInRect(g, L"勾选的卡才会带进游戏（英雄卡也在下面，勾几张就带几张）",
                   RectF(S(220.f), S(44.f), W - S(300.f), S(24.f)),
                   g_fonts.footer.get(), Color(255, 0x78, 0x78, 0x82), StringAlignmentFar);

    // 已选计数
    DrawTextInRect(g, L"已选 " + std::to_wstring(DeckPickedCount()) + L" / 8 张",
                   RectF(S(36.f), S(104.f), S(360.f), S(24.f)),
                   g_fonts.status.get(), Color(255, 0x9e, 0xe8, 0xa2), StringAlignmentNear);

    // 卡牌网格
    for (int k = 0; k < g_deckCardCount; ++k) {
        const RectF tile = DeckTileRect(k);
        const bool on = g_deckPicked[k];
        const bool hot = (g_deckHoverTile == k);
        GraphicsPath path;
        AddRoundRect(path, tile.X, tile.Y, tile.Width, tile.Height, S(12.f));
        SolidBrush fill(on ? Color(255, 0x1d, 0x2a, 0x1d) : Color(255, 0x16, 0x16, 0x1c));
        g.FillPath(&fill, &path);
        Pen border(on ? Color(255, 0x86, 0xd9, 0x8a)
                      : (hot ? Color(255, 0x4a, 0x4a, 0x56) : Color(255, 0x2a, 0x2a, 0x32)),
                   on ? g_scale * 2.f : g_scale);
        g.DrawPath(&border, &path);

        const RectF chk = DeckCheckRect(tile);
        GraphicsPath cp;
        AddRoundRect(cp, chk.X, chk.Y, chk.Width, chk.Height, S(6.f));
        SolidBrush cfill(on ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x22, 0x22, 0x28));
        g.FillPath(&cfill, &cp);
        if (on) DrawTextInRect(g, L"✓", chk, g_fonts.button.get(), Color(255, 0x12, 0x1a, 0x16));

        // 卡面缩略图
        Bitmap* thumb = (k < (int)g_deckThumbs.size()) ? g_deckThumbs[k].get() : nullptr;
        const float tx = tile.X + S(48.f);
        if (thumb) {
            const float scale = std::min(S(46.f) / thumb->GetWidth(), S(46.f) / thumb->GetHeight());
            const float dw = thumb->GetWidth() * scale, dh = thumb->GetHeight() * scale;
            g.DrawImage(thumb, tx + (S(46.f) - dw) * 0.5f, tile.Y + (tile.Height - dh) * 0.5f, dw, dh);
        } else {
            DrawTextInRect(g, L"无图", RectF(tx, tile.Y, S(46.f), tile.Height),
                           g_fonts.footer.get(), Color(255, 0x4a, 0x4a, 0x54));
        }

        // 名称 + 费用
        const float textX = tx + S(54.f);
        const float w = tile.X + tile.Width - textX - S(8.f);
        DrawTextInRect(g, std::wstring(g_deckCards[k].name),
                       RectF(textX, tile.Y + S(9.f), w, S(26.f)),
                       g_fonts.status.get(), on ? Color(255, 0xe6, 0xe6, 0xec) : Color(255, 0xb0, 0xb0, 0xba),
                       StringAlignmentNear);
        DrawTextInRect(g, L"费用 " + std::to_wstring(g_deckCards[k].cost),
                       RectF(textX, tile.Y + S(35.f), w, S(20.f)),
                       g_fonts.footer.get(), Color(255, 0x9e, 0xe8, 0xa2), StringAlignmentNear);
    }

    // 携带英雄 开关
    {
        const RectF r = DeckHeroRect();
        const bool on = g_deckHeroPicked;
        const bool hot = (g_deckHoverHero == 1);
        GraphicsPath path;
        AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(12.f));
        SolidBrush fill(g_deckHeroAvail ? (on ? Color(255, 0x1d, 0x2a, 0x1d) : Color(255, 0x16, 0x16, 0x1c))
                                        : Color(255, 0x12, 0x12, 0x16));
        g.FillPath(&fill, &path);
        Pen border(g_deckHeroAvail ? (on ? Color(255, 0x86, 0xd9, 0x8a)
                                          : (hot ? Color(255, 0x4a, 0x4a, 0x56) : Color(255, 0x2a, 0x2a, 0x32)))
                                    : Color(255, 0x22, 0x22, 0x28), g_scale);
        g.DrawPath(&border, &path);
        const RectF chk(r.X + S(12.f), r.Y + (r.Height - S(26.f)) * 0.5f, S(26.f), S(26.f));
        GraphicsPath cp;
        AddRoundRect(cp, chk.X, chk.Y, chk.Width, chk.Height, S(6.f));
        SolidBrush cfill((on && g_deckHeroAvail) ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x22, 0x22, 0x28));
        g.FillPath(&cfill, &cp);
        if (on && g_deckHeroAvail) DrawTextInRect(g, L"✓", chk, g_fonts.button.get(), Color(255, 0x12, 0x1a, 0x16));
        DrawTextInRect(g, L"携带全部英雄（仅在上面一张卡都没勾选时生效；想带指定英雄请直接在上面勾选）",
                       RectF(chk.X + S(34.f), r.Y, r.Width - S(46.f), r.Height),
                       g_fonts.status.get(),
                       g_deckHeroAvail ? Color(255, 0xe6, 0xe6, 0xec) : Color(255, 0x5a, 0x5a, 0x64),
                       StringAlignmentNear);
    }

    // 底部状态提示
    DrawTextInRect(g, g_deckTip.empty()
                            ? L"提示：点卡片左侧方框勾选，最多 8 张；勾了就只带勾的这些。点「保存并应用」后开始游戏生效"
                            : g_deckTip,
                   RectF(S(36.f), kDeckH * g_scale - S(92.f), kDeckW * g_scale - S(72.f), S(24.f)),
                   g_fonts.footer.get(), Color(255, 0x8a, 0x92, 0x9e), StringAlignmentNear);

    // 两个按钮（复用设计弹窗的版式：返回 / 保存并应用）
    DrawPopupButton(g, CloseButtonRect(), L"返回",
                    Color(255, 0x24, 0x24, 0x2c), Color(255, 0xb8, 0xb8, 0xc2));
    DrawPopupButton(g, ApplyButtonRect(), L"保存并应用",
                    Color(255, 0x86, 0xd9, 0x8a), Color(255, 0x12, 0x1a, 0x16));
}

enum DeckTarget { DECK_NONE = 0, DECK_CLOSE, DECK_APPLY, DECK_HERO, DECK_TILE };

static DeckTarget HitTestDeck(const PointF& p, int& index) {
    index = -1;
    if (CloseButtonRect().Contains(p)) return DECK_CLOSE;
    if (ApplyButtonRect().Contains(p)) return DECK_APPLY;
    if (DeckHeroRect().Contains(p))    return DECK_HERO;
    for (int k = 0; k < g_deckCardCount; ++k)
        if (DeckTileRect(k).Contains(p)) { index = k; return DECK_TILE; }
    return DECK_NONE;
}

static void SaveDeckToDisk() {
    CreateDirectoryW(L"img", nullptr);
    CreateDirectoryW(L"img\\custom", nullptr);
    std::ofstream out("img\\custom\\deck.txt", std::ios::out | std::ios::trunc);
    if (!out) { g_deckTip = L"保存失败：无法写入 deck.txt"; return; }
    out << "hero=" << (g_deckHeroPicked ? 1 : 0) << "\n";
    int saved = 0;
    for (int k = 0; k < g_deckCardCount && saved < 8; ++k) {
        if (g_deckPicked[k]) { out << "type=" << g_deckCards[k].type << "\n"; saved++; }
    }
    out.close();
    LoadDeckFromDisk();   // 同步到内存全局，game 启动前即可生效
    g_deckTip = L"已保存出战卡组（" + std::to_wstring(DeckPickedCount()) + L" 张" +
                (g_deckHeroPicked && DeckPickedCount() == 0 ? L" + 全部英雄" : L"") +
                L"）—— 开始游戏后生效";
}

static LRESULT CALLBACK DeckWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem) { EndPaint(hwnd, &ps); return 0; }
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        if (!bmp) { DeleteDC(mem); EndPaint(hwnd, &ps); return 0; }
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        { Graphics gg(mem); DrawDeckScene(gg); }
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old) SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&track);
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1;
        const DeckTarget target = HitTestDeck(point, index);
        const int hoverTile = (target == DECK_TILE) ? index : -1;
        const int hoverHero = (target == DECK_HERO) ? 1 : -1;
        if (hoverTile != g_deckHoverTile || hoverHero != g_deckHoverHero) {
            g_deckHoverTile = hoverTile;
            g_deckHoverHero = hoverHero;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_deckHoverTile != -1 || g_deckHoverHero != -1) {
            g_deckHoverTile = -1;
            g_deckHoverHero = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1;
        switch (HitTestDeck(point, index)) {
        case DECK_CLOSE:
            DestroyWindow(hwnd);
            break;
        case DECK_APPLY:
            SaveDeckToDisk();
            g_deckTip.clear();
            DestroyWindow(hwnd);
            break;
        case DECK_HERO:
            if (!g_deckHeroAvail) {
                g_deckTip = L"尚未在「设计角色 → 英雄」上传英雄图，暂不能携带英雄";
                RefreshDeck();
            } else {
                g_deckHeroPicked = !g_deckHeroPicked;
                RefreshDeck();
            }
            break;
        case DECK_TILE:
            if (!g_deckPicked[index] && DeckPickedCount() >= 8) {
                g_deckTip = L"最多只能选择 8 张卡牌，请先取消一张再勾选";
                RefreshDeck();
            } else {
                g_deckPicked[index] = !g_deckPicked[index];
                g_deckTip.clear();
                RefreshDeck();
            }
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const PointF clientPoint((REAL)point.x, (REAL)point.y);
            int index = -1;
            SetCursor(LoadCursor(nullptr, HitTestDeck(clientPoint, index) != DECK_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_deckHwnd == hwnd) g_deckHwnd = nullptr;
        if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowCharMenuWindow();
        else ShowLauncherWindow();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RefreshDeck() {
    if (g_deckHwnd) InvalidateRect(g_deckHwnd, nullptr, FALSE);
}

static void OpenDeckPopup() {
    if (g_deckHwnd && IsWindow(g_deckHwnd)) {
        ShowWindow(g_deckHwnd, SW_RESTORE);
        SetForegroundWindow(g_deckHwnd);
        return;
    }

    // 枚举可用卡牌（内置 + 导入角色 + 英雄），并载入缩略图
    LoadBioStats();   // 让 cardCostForType 能拿到自定义卡费用
    g_deckCardCount = CollectDeckCardInfos(g_deckCards, 64);
    g_deckThumbs.clear();
    for (int k = 0; k < g_deckCardCount; ++k) {
        g_deckThumbs.push_back(nullptr);
        if (g_deckCards[k].imgPath[0]) {
            std::unique_ptr<Bitmap> b = LoadScaledImage(std::wstring(g_deckCards[k].imgPath), 64);
            if (b) g_deckThumbs[k] = std::move(b);
        }
    }
    // 回显已保存的卡组：把 g_deckTypes 里存在的 type 勾上
    g_deckPicked.assign(g_deckCardCount, false);
    for (int k = 0; k < g_deckCardCount; ++k)
        for (int j = 0; j < g_deckCount; ++j)
            if (g_deckTypes[j] == g_deckCards[k].type) { g_deckPicked[k] = true; break; }

    // 英雄图是否可用（决定开关可用性 + 预选）：13 个英雄里任意 1 个配了图就算可用
    g_deckHeroAvail = false;
    for (int h = 0; h < MAX_HEROES && !g_deckHeroAvail; ++h) {
        std::wstring unused;
        if (HeroFramePath(h, 0, 1, unused) || HeroFramePath(h, 1, 1, unused) ||
            HeroCardPathOnDisk(h, unused))
            g_deckHeroAvail = true;
    }
    g_deckHeroPicked = g_heroEnabled && g_deckHeroAvail;
    g_deckHoverTile = -1;
    g_deckHoverHero = -1;
    g_deckTip.clear();

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DeckWndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kDeckClass;
        RegisterClassExW(&wc);
        s_registered = true;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{ 0, 0, (LONG)(kDeckW * g_scale), (LONG)(kDeckH * g_scale) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;

    // 在角色设计子菜单上方居中
    int wx, wy;
    HWND centerOn = (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ? g_charMenuHwnd
                  : (g_mainHwnd && IsWindow(g_mainHwnd))        ? g_mainHwnd
                  : nullptr;
    if (centerOn) {
        RECT mw; GetWindowRect(centerOn, &mw);
        wx = mw.left + ((mw.right - mw.left) - ww) / 2;
        wy = mw.top  + ((mw.bottom - mw.top) - wh) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        wx = wa.left + ((wa.right - wa.left) - ww) / 2;
        wy = wa.top  + ((wa.bottom - wa.top) - wh) / 2;
    }

    g_deckHwnd = CreateWindowExW(0, kDeckClass, L"出战卡组",
                                 style, wx, wy, ww, wh, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);

    if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowWindow(g_charMenuHwnd, SW_HIDE);
    else if (g_mainHwnd) ShowWindow(g_mainHwnd, SW_HIDE);

    if (g_deckHwnd) { ShowWindow(g_deckHwnd, SW_SHOW); UpdateWindow(g_deckHwnd); }
}

static void OnCharMenuDeck() {
    SetActionStatus(L"出战卡组", L"OnCharMenuDeck");
    OpenDeckPopup();
}

// 启动时读 deck.txt 到内存全局；没有文件则保持 g_deckCount=0（game 回退「全部可用」）
static void LoadDeckFromDisk() {
    g_deckCount = 0;
    g_heroEnabled = false;
    std::ifstream in("img\\custom\\deck.txt");
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (unsigned char)line.back() <= ' ') line.pop_back();
        size_t b = 0; while (b < line.size() && (unsigned char)line[b] <= ' ') ++b;
        if (b > 0) line.erase(0, b);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.rfind("hero=", 0) == 0) {
            g_heroEnabled = (std::atoi(line.c_str() + 5) != 0);
        } else if (line.rfind("type=", 0) == 0) {
            if (g_deckCount >= 8) continue;
            int t = std::atoi(line.c_str() + 5);
            // 兼容旧卡组：以前英雄只有一张、type 就是 HERO_TYPE(300)，
            // 现在第 n 个英雄是 301..313，旧的 300 一律当作 1 号英雄。
            if (t == 300) t = 301;
            g_deckTypes[g_deckCount++] = t;
        }
    }
}

// =====================================================================
// 【接口位置 3】小豪 —— 跳转打开 demo.py（智能体工作台）
// ---------------------------------------------------------------------
// 实现 : 在 exe 同目录定位 demo.py（与源码同目录打包），再用系统 Python
//        解释器直接启动。解释器按 PATH 依次查找：
//          pythonw.exe（首选，无黑色控制台窗口）
//          python.exe（备选，会带一个控制台窗口便于看报错）
//          py.exe    （Python 官方启动器，兜底）
//        demo.py 在独立进程中运行，不影响启动器窗口；找不到解释器或
//        启动失败时弹窗提示。
// 依赖 : demo.py 需要 requests 库 —— 若窗口一闪而过/无窗口，
//        在命令行执行 : pip install requests
// =====================================================================
// =====================================================================
// 启动 demo.py（智能体工作台）的公共实现
// ---------------------------------------------------------------------
// hideHwnd  : 启动期间要隐藏的窗口（启动器或子菜单窗口）
// errOwner  : 失败时 MessageBox 的父窗口
// 返回值    : true=进程已成功运行并等待结束；false=未启动（已弹错误提示）
//
// 调用方负责在返回 true 后"恢复"自己的上层窗口：
//   - 启动器「小豪」按钮 → ShowLauncherWindow()
//   - 子菜单「智能体」按钮 → ShowCharMenuWindow()
// =====================================================================
static bool LaunchDemoPyAndWait(HWND hideHwnd, HWND errOwner) {
    // 1) 定位 demo.py：优先 exe 所在目录，找不到再退回当前工作目录
    std::wstring demoPath;
    {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
            std::wstring dir(exePath);
            const size_t sep = dir.find_last_of(L"\\/");
            if (sep != std::wstring::npos)
                demoPath = dir.substr(0, sep) + L"\\demo.py";
        }
        if (demoPath.empty() ||
            GetFileAttributesW(demoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            demoPath = L"demo.py";                       // 回退：当前工作目录
        }
    }

    // demo.py 所在目录作为子进程工作目录，保证脚本内相对路径可用
    std::wstring demoDir = L".";
    {
        const size_t sep = demoPath.find_last_of(L"\\/");
        if (sep != std::wstring::npos) demoDir = demoPath.substr(0, sep);
    }

    // 2) 按 PATH 查找 Python 解释器
    wchar_t pythonExe[MAX_PATH]{};
    if (!SearchPathW(nullptr, L"pythonw.exe", nullptr, MAX_PATH, pythonExe, nullptr) &&
        !SearchPathW(nullptr, L"python.exe",  nullptr, MAX_PATH, pythonExe, nullptr) &&
        !SearchPathW(nullptr, L"py.exe",      nullptr, MAX_PATH, pythonExe, nullptr)) {
        MessageBoxW(errOwner,
                    L"没有找到 Python 解释器。\n"
                    L"请先安装 Python 3 并勾选「Add Python to PATH」，再点击此按钮。",
                    L"智能体 · 启动失败", MB_OK | MB_ICONWARNING);
        return false;
    }

    // 3) 启动 demo.py
    //    【注意】命令行里必须把「解释器」和「脚本」都写上：
    //    CreateProcessW 不会自动补 argv[0]，如果命令行只有脚本路径，python 会把
    //    它当成 argv[0]（程序名）而找不到要运行的脚本，于是进入交互模式；
    //    pythonw.exe 没有控制台，会静默退出 —— 表现就是"点了没反应/打不开"。
    std::wstring cmdLine = L"\"" + std::wstring(pythonExe) + L"\" \"" + demoPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');                       // CreateProcessW 可能就地修改命令行，必须是可写缓冲

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(pythonExe, cmdBuf.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, demoDir.c_str(), &si, &pi)) {
        MessageBoxW(errOwner,
                    L"启动 demo.py 失败，请检查 Python 环境与文件是否完整。",
                    L"智能体 · 启动失败", MB_OK | MB_ICONERROR);
        return false;
    }

    if (hideHwnd) ShowWindow(hideHwnd, SW_HIDE);
    WaitForSingleObject(pi.hProcess, INFINITE);    // 阻塞直至 demo.py 窗口关闭
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void OnXiaoHaoClicked() {
    SetActionStatus(L"小豪 · 智能体工作台", L"OnXiaoHaoClicked");
    if (LaunchDemoPyAndWait(g_mainHwnd, g_mainHwnd)) ShowLauncherWindow();
}


// ---------------------------------------------------------------------
// 按钮小图标（26*S 芯片内绘制，纯矢量）
// ---------------------------------------------------------------------
static void DrawIconPlay(Graphics& g, REAL x, REAL y, REAL s, const Color& c) {
    SolidBrush br(c);
    PointF pts[3] = { PointF(x + 9.5f*s, y + 7.5f*s),
                      PointF(x + 9.5f*s, y + 18.5f*s),
                      PointF(x + 19.f*s,  y + 13.f*s) };
    g.FillPolygon(&br, pts, 3);
}

// 设计角色 · 人物剪影（头 + 肩，与播放图标同规格的矢量小图标）
static void DrawIconCharacter(Graphics& g, REAL x, REAL y, REAL s, const Color& c) {
    SolidBrush br(c);
    g.FillEllipse(&br, x + 9.f*s, y + 5.5f*s, 8.f*s, 8.f*s);   // 头
    GraphicsPath body;                                        // 肩（上半圆拱）
    body.AddArc(x + 5.5f*s, y + 15.5f*s, 15.f*s, 11.f*s, 180.f, 180.f);
    body.CloseFigure();
    g.FillPath(&br, &body);
}

// 小豪 · 四角星芒（智能体/灵感的意象，与播放图标同规格的矢量小图标）
static void DrawIconSpark(Graphics& g, REAL x, REAL y, REAL s, const Color& c) {
    SolidBrush br(c);
    const REAL cx = x + 13.f*s, cy = y + 13.f*s;
    const REAL r1 = 8.5f*s, r2 = 2.4f*s;      // 长轴 / 短轴
    PointF pts[8] = {
        PointF(cx,      cy - r1),
        PointF(cx + r2, cy - r2),
        PointF(cx + r1, cy),
        PointF(cx + r2, cy + r2),
        PointF(cx,      cy + r1),
        PointF(cx - r2, cy + r2),
        PointF(cx - r1, cy),
        PointF(cx - r2, cy - r2),
    };
    g.FillPolygon(&br, pts, 8);
}

// ---------------------------------------------------------------------
// 按钮绘制（悬停渐变 / 按下位移 / 外发光）—— 全部物理像素
// ---------------------------------------------------------------------
static void DrawButton(Graphics& g, MenuButton& b) {
    const float t = b.hover;
    RectF r = b.rect;
    if (b.pressed) r.Offset(0.f, 1.f * g_scale);

    const Color& acc = kAccents[b.accent];

    // 悬停外发光
    if (t > 0.02f) {
        GraphicsPath glow;
        AddRoundRect(glow, r.X - 8.f*g_scale*t, r.Y - 8.f*g_scale*t,
                     r.Width + 16.f*g_scale*t, r.Height + 16.f*g_scale*t, 18.f*g_scale);
        SolidBrush gb(Color((BYTE)(26.f * t), 0x9e, 0xe8, 0xa2));
        g.FillPath(&gb, &glow);
    }

    // 主体
    GraphicsPath body;
    AddRoundRect(body, r.X, r.Y, r.Width, r.Height, 12.f * g_scale);
    SolidBrush fill(LerpColor(Color(255, 0x26, 0x26, 0x2c),
                              Color(255, 0x2f, 0x2f, 0x39), t));
    g.FillPath(&fill, &body);

    Pen border(LerpColor(Color(255, 0x3a, 0x3a, 0x44),
                         Color(acc.GetR(), acc.GetG(), acc.GetB(), 190), t), g_scale);
    g.DrawPath(&border, &body);

    // 图标芯片
    const REAL chip = 26.f * g_scale;
    const REAL chipX = r.X + 22.f * g_scale, chipY = r.Y + (r.Height - chip) * 0.5f;
    GraphicsPath chipPath;
    AddRoundRect(chipPath, chipX, chipY, chip, chip, 8.f * g_scale);
    SolidBrush chipFill(Color((BYTE)(34.f + 26.f * t), acc.GetR(), acc.GetG(), acc.GetB()));
    g.FillPath(&chipFill, &chipPath);
    Pen chipPen(Color((BYTE)(80.f + 80.f * t), acc.GetR(), acc.GetG(), acc.GetB()), g_scale);
    g.DrawPath(&chipPen, &chipPath);

    const Color iconC(255, acc.GetR(), acc.GetG(), acc.GetB());
    if      (b.icon == 1) DrawIconCharacter(g, chipX, chipY, g_scale, iconC);  // 设计角色
    else if (b.icon == 2) DrawIconSpark(g, chipX, chipY, g_scale, iconC);      // 小豪
    else                  DrawIconPlay(g, chipX, chipY, g_scale, iconC);       // 开始游戏

    // 文字（左对齐，垂直居中）
    RectF tr(r.X + 64.f * g_scale, r.Y, r.Width - 92.f * g_scale, r.Height);
    DrawTextInRect(g, b.label, tr, g_fonts.button.get(),
                   LerpColor(Color(255, 0xd4, 0xd4, 0xdc), Color(255, 255, 255, 255), t),
                   StringAlignmentNear);

    // 右侧箭头
    RectF cr(r.X + r.Width - 42.f * g_scale, r.Y, 30.f * g_scale, r.Height);
    DrawTextInRect(g, L"›", cr, g_fonts.chev.get(),
                   LerpColor(Color(255, 0x55, 0x55, 0x60), Color(255, 0xb8, 0xb8, 0xc2), t));
}

// ---------------------------------------------------------------------
// 整体场景绘制（全部物理像素，无世界变换）
// ---------------------------------------------------------------------
static void DrawScene(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const float W = kDesignW * g_scale, H = kDesignH * g_scale;

    if (g_mode == AppMode::Game) {
        //DrawGameScene(g);
        return;
    }

    // 背景：垂直渐变
    RectF full(0.f, 0.f, W, H);
    LinearGradientBrush bg(full, Color(255, 0x14, 0x14, 0x18),
                           Color(255, 0x1f, 0x1f, 0x26), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);

    // 标题区
    DrawTextInRect(g, L"====================================",
                   RectF(0.f, S(118.f), W, S(22.f)), g_fonts.sub.get(),
                   Color(255, 0x8a, 0x8a, 0x94));
    DrawTextInRect(g, L"塔防小游戏",
                   RectF(0.f, S(146.f), W, S(64.f)), g_fonts.title.get(),
                   Color(255, 0xf2, 0xf2, 0xf5));

    // 标题下的强调短线
    {
        GraphicsPath bar;
        AddRoundRect(bar, W * 0.5f - S(28.f), S(224.f), S(56.f), S(3.f), S(1.5f));
        SolidBrush barBr(Color(255, 0x86, 0xd9, 0x8a));
        g.FillPath(&barBr, &bar);
    }

    // 主按钮
    for (auto& b : g_buttons) DrawButton(g, b);

    // 状态栏
    std::wstring st = g_status.empty()
        ? L"就绪 · 点击任意按钮 —— 各接口位置见 main.cpp 中「接口位置」注释"
        : g_status;
    DrawTextInRect(g, st, RectF(0.f, H - S(86.f), W, S(22.f)),
                   g_fonts.status.get(), Color(255, 0x78, 0x78, 0x82));

    // 页脚分隔线与信息
    Pen line(Color(14, 255, 255, 255));
    g.DrawLine(&line, 0.f, H - S(46.f), W, H - S(46.f));
    DrawTextInRect(g, L"v0.1.0 · 预览版",
                   RectF(S(20.f), H - S(40.f), S(220.f), S(24.f)), g_fonts.footer.get(),
                   Color(255, 0x5e, 0x5e, 0x68), StringAlignmentNear);
    DrawTextInRect(g, L"C++ · Win32 · GDI+",
                   RectF(W - S(240.f), H - S(40.f), S(220.f), S(24.f)), g_fonts.footer.get(),
                   Color(255, 0x5e, 0x5e, 0x68), StringAlignmentFar);
}

// =====================================================================
// 角色设计 · 子菜单窗口（4 按钮：已有卡牌 / 添加角色 / 智能体 / 返回主菜单）
//   点击启动器「设计角色」→ 隐藏启动器 → 显示子菜单
// =====================================================================
static const wchar_t* kCharMenuClass = L"PvZCharMenuWnd";
static std::vector<MenuButton> g_charButtons;

static void OnCharMenuExistingCards();
static void OnCharMenuAddCharacter();
static void OnCharMenuAgent();
static void OnCharMenuBack();

static void ShowCharMenuWindow() {
    if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) {
        ShowWindow(g_charMenuHwnd, SW_SHOW);
        SetForegroundWindow(g_charMenuHwnd);
        InvalidateRect(g_charMenuHwnd, nullptr, FALSE);
    }
}

static void CreateCharButtons(float s) {
    g_charButtons.clear();
    const float bw = 360.f * s, bh = 60.f * s, gap = 16.f * s;
    const float x = (kDesignW * s - bw) * 0.5f;
    const float y0 = 184.f * s;        // 5 个按钮整体居中
    auto make = [=](const wchar_t* label, const wchar_t* fn, int icon,
                    float y, void (*cb)()) {
        MenuButton b;
        b.label = label; b.funcName = fn; b.accent = 0; b.icon = icon;
        b.rect = RectF(x, y, bw, bh); b.onClick = cb;
        g_charButtons.push_back(std::move(b));
    };
    make(L"已有卡牌",   L"OnCharMenuExistingCards", 1, y0,                  &OnCharMenuExistingCards);
    make(L"添加角色",   L"OnCharMenuAddCharacter",  1, y0 + (bh + gap),     &OnCharMenuAddCharacter);
    make(L"出战卡组",   L"OnCharMenuDeck",          1, y0 + (bh + gap) * 2, &OnCharMenuDeck);
    make(L"智能体",     L"OnCharMenuAgent",         2, y0 + (bh + gap) * 3, &OnCharMenuAgent);
    make(L"返回主菜单", L"OnCharMenuBack",          0, y0 + (bh + gap) * 4, &OnCharMenuBack);
}

static void DrawCharMenuScene(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const float W = kDesignW * g_scale, H = kDesignH * g_scale;
    RectF full(0, 0, W, H);
    LinearGradientBrush bg(full, Color(255, 0x14, 0x14, 0x18),
                           Color(255, 0x1f, 0x1f, 0x26), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);
    DrawTextInRect(g, L"角色设计", RectF(0, S(96.f), W, S(64.f)),
                   g_fonts.title.get(), Color(255, 0xf2, 0xf2, 0xf2));
    {
        GraphicsPath bar;
        AddRoundRect(bar, W * 0.5f - S(28.f), S(172.f), S(56.f), S(3.f), S(1.5f));
        SolidBrush br(Color(255, 0x86, 0xd9, 0x8a));
        g.FillPath(&br, &bar);
    }
    for (auto& b : g_charButtons) DrawButton(g, b);
    DrawTextInRect(g, L"已有卡牌 · 添加角色 · 出战卡组 · 智能体工作台",
                   RectF(0, H - S(86.f), W, S(22.f)),
                   g_fonts.status.get(), Color(255, 0x78, 0x78, 0x82));
}

static LRESULT CALLBACK CharMenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_charMenuHwnd = hwnd;
        CreateCharButtons(g_scale);
        SetTimer(hwnd, 2, 16, nullptr);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem) { EndPaint(hwnd, &ps); return 0; }
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        if (!bmp) { DeleteDC(mem); EndPaint(hwnd, &ps); return 0; }
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        { Graphics gg(mem); DrawCharMenuScene(gg); }
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old) SelectObject(mem, old);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == 2) {
            bool dirty = false;
            for (auto& b : g_charButtons) {
                float target = b.over ? 1.f : 0.f;
                if (std::fabs(b.hover - target) > 0.004f) {
                    b.hover += (target - b.hover) * 0.22f; dirty = true;
                } else if (b.hover != target) { b.hover = target; dirty = true; }
            }
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        PointF p((REAL)pt.x, (REAL)pt.y);
        int hit = -1;
        for (size_t i = 0; i < g_charButtons.size(); ++i)
            if (g_charButtons[i].rect.Contains(p)) { hit = (int)i; break; }
        bool changed = false;
        for (size_t i = 0; i < g_charButtons.size(); ++i) {
            bool over = ((int)i == hit);
            if (g_charButtons[i].over != over) { g_charButtons[i].over = over; changed = true; }
        }
        if (changed) {
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        if (GetCapture() == hwnd) {
            for (auto& b : g_charButtons)
                b.pressed = b.pressed && hit >= 0 && b.rect.Contains(p);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (GetCapture() == hwnd) return 0;
        for (auto& b : g_charButtons) { b.over = false; b.pressed = false; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        PointF p((REAL)pt.x, (REAL)pt.y);
        for (size_t i = 0; i < g_charButtons.size(); ++i)
            if (g_charButtons[i].rect.Contains(p)) {
                g_charButtons[i].pressed = true; SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE); break;
            }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        PointF p((REAL)pt.x, (REAL)pt.y);
        for (auto& b : g_charButtons) {
            if (b.pressed && b.rect.Contains(p) && b.onClick) b.onClick();
            b.pressed = false;
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            PointF p((REAL)pt.x, (REAL)pt.y);
            int hit = -1;
            for (size_t i = 0; i < g_charButtons.size(); ++i)
                if (g_charButtons[i].rect.Contains(p)) { hit = (int)i; break; }
            SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_charMenuHwnd == hwnd) g_charMenuHwnd = nullptr;
        g_charButtons.clear();
        KillTimer(hwnd, 2);
        ShowLauncherWindow();   // 返回主菜单
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenCharMenu() {
    if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) {
        ShowWindow(g_charMenuHwnd, SW_RESTORE);
        SetForegroundWindow(g_charMenuHwnd);
        return;
    }
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = CharMenuWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kCharMenuClass;
        RegisterClassExW(&wc);
        s_reg = true;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, (LONG)(kDesignW * g_scale), (LONG)(kDesignH * g_scale) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int wx, wy;
    if (g_mainHwnd && IsWindow(g_mainHwnd)) {
        RECT mw; GetWindowRect(g_mainHwnd, &mw);
        wx = mw.left + ((mw.right - mw.left) - ww) / 2;
        wy = mw.top + ((mw.bottom - mw.top) - wh) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        wx = wa.left + ((wa.right - wa.left) - ww) / 2;
        wy = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    }
    HWND hwnd = CreateWindowExW(0, kCharMenuClass, L"角色设计",
        style, wx, wy, ww, wh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_mainHwnd) ShowWindow(g_mainHwnd, SW_HIDE);   // 隐藏启动器
    if (hwnd) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }
}

// ---- 子菜单按钮动作 ----
static void OnCharMenuBack() {
    if (g_charMenuHwnd) DestroyWindow(g_charMenuHwnd);   // → WM_DESTROY → ShowLauncherWindow
}
static void OnCharMenuAddCharacter() {
    OpenDesignCharacterPopup();   // 已有函数：隐藏 CharMenu，打开设计工坊，关闭后恢复 CharMenu
}
static void OnCharMenuExistingCards() {
    OpenCardsPopup();              // 隐藏 CharMenu，打开已有卡牌弹窗
}
static void OnCharMenuAgent() {
    SetActionStatus(L"智能体 · 工作台", L"OnCharMenuAgent");
    if (LaunchDemoPyAndWait(g_charMenuHwnd, g_charMenuHwnd)) ShowCharMenuWindow();
}

// =====================================================================
// 已有卡牌弹窗 —— 分「我方卡牌」「敌人卡牌」两组浏览，仅可删除自定义卡牌
//   内置卡牌（射手 / 收集者）：不可删除
//   我方卡牌 = 英雄 1~13（在「设计角色 → 英雄」里上传）
//   敌人卡牌 = 敌人生物（在「设计角色 → 敌人生物」里上传）
//   敌人删除时把后续文件前移补位（编号保持紧凑，界面上也更好看）；
//   英雄是固定编号，删除即清空该英雄的全部素材，不需要补位。
//   注：game.cpp 的 loadCustomBiologys 会扫完 13 个编号并跳过空号，
//   所以中间有空号也不会导致敌人加载不到。
// =====================================================================
static HWND g_cardsPopupHwnd = nullptr;
static const wchar_t* kCardsPopupClass = L"PvZCardsPopupWnd";

// 两组：0 = 我方卡牌（英雄），1 = 敌人卡牌
enum { CARDS_GROUP_MY = 0, CARDS_GROUP_ENEMY = 1, CARDS_GROUP_COUNT = 2 };
static const wchar_t* kCardsGroupNames[CARDS_GROUP_COUNT] = { L"我方卡牌", L"敌人卡牌" };
static const wchar_t* kCardsGroupHints[CARDS_GROUP_COUNT] = {
    L"英雄 1~13：在「设计角色 → 英雄」里上传；点 × 会连同该英雄的动画帧、卡面、子弹图一起删除",
    L"敌人生物：在「设计角色 → 敌人生物」里上传（行走 6 帧 / 攻击 8 帧 + 1 张卡牌图），"
    L"进入游戏后从右侧登场；点 × 会连同该敌人的动画帧和卡牌图一起删除",
};

static std::vector<std::unique_ptr<Bitmap>> g_cardsBuiltIn;
static std::vector<std::wstring>            g_cardsBuiltInNames;
static std::vector<std::unique_ptr<Bitmap>> g_cardsCustom[CARDS_GROUP_COUNT];
static std::vector<std::wstring>            g_cardsCustomPaths[CARDS_GROUP_COUNT];
static std::vector<std::wstring>            g_cardsCustomNames[CARDS_GROUP_COUNT];
static int                                   g_cardsTab = CARDS_GROUP_MY;
static int                                   g_cardsHoverSlot = -1;
static std::wstring                          g_cardsTip;

static void LoadCardsForDisplay() {
    g_cardsBuiltIn.clear();
    g_cardsBuiltInNames.clear();
    struct { const wchar_t* path; const wchar_t* name; } builtin[] = {
        { L"img\\Cards\\card_1.png", L"射手" },
        { L"img\\Cards\\card_2.png", L"收集者"   },
    };
    for (auto& b : builtin) {
        if (GetFileAttributesW(b.path) == INVALID_FILE_ATTRIBUTES) continue;
        auto thumb = LoadScaledImage(b.path, 256);
        if (thumb) { g_cardsBuiltIn.push_back(std::move(thumb)); g_cardsBuiltInNames.push_back(b.name); }
    }

    // ---- 我方卡牌：13 个英雄，优先用卡面，没配卡面就用攻击首帧 ----
    g_cardsCustom[CARDS_GROUP_MY].clear();
    g_cardsCustomPaths[CARDS_GROUP_MY].clear();
    g_cardsCustomNames[CARDS_GROUP_MY].clear();
    for (int h = 0; h < MAX_HEROES; ++h) {
        std::wstring path;
        if (!HeroCardPathOnDisk(h, path) && !HeroFramePath(h, 0, 1, path) &&
            !HeroFramePath(h, 1, 1, path))
            continue;                                  // 这个英雄还没配图，不占位
        auto thumb = LoadScaledImage(path, 256);
        if (!thumb) continue;
        g_cardsCustom[CARDS_GROUP_MY].push_back(std::move(thumb));
        g_cardsCustomPaths[CARDS_GROUP_MY].push_back(path);
        g_cardsCustomNames[CARDS_GROUP_MY].push_back(L"英雄" + std::to_wstring(h + 1));
    }

    // ---- 敌人卡牌：每名敌人一张卡 ----
    // 封面优先级：敌方卡面 enemy{N}_card.png > 行走第 1 帧 > 攻击第 1 帧。
    // 注意：敌人编号可以跳着配（中间有空号），所以这里必须扫完 13 个编号并跳过空号，
    // 不能遇到空号就 break —— 否则后面的敌人一个都显示不出来。
    g_cardsCustom[CARDS_GROUP_ENEMY].clear();
    g_cardsCustomPaths[CARDS_GROUP_ENEMY].clear();
    g_cardsCustomNames[CARDS_GROUP_ENEMY].clear();
    for (int n = 0; n < kMaxBioSlots; ++n) {
        std::wstring path;
        const bool hasCard = EnemyCardPathOnDisk(n, path);
        if (!hasCard && !EnemyFramePath(n, 0, 1, path) && !EnemyFramePath(n, 1, 1, path))
            continue;                                    // 这一号没配 → 跳过，继续看下一号
        auto thumb = LoadScaledImage(path, 256);
        if (!thumb) continue;
        g_cardsCustom[CARDS_GROUP_ENEMY].push_back(std::move(thumb));
        g_cardsCustomPaths[CARDS_GROUP_ENEMY].push_back(path);
        g_cardsCustomNames[CARDS_GROUP_ENEMY].push_back(L"敌人" + std::to_wstring(n + 1));
    }
}

// 删除我方卡牌 = 清空该英雄的全部素材（动画帧 + 卡面 + 子弹图）
static void DeleteHeroCard(int heroIndex) {
    if (heroIndex < 0 || heroIndex >= MAX_HEROES) return;
    for (int which = 0; which < 2; ++which)
        for (int f = 1; f <= HERO_FRAMES; ++f)
            DeleteFileW(HeroFilePath(heroIndex, which, f).c_str());
    DeleteFileW(HeroCardFilePath(heroIndex).c_str());
    DeleteFileW(HeroBulletFilePath(heroIndex).c_str());
}

// 删掉第 enemyIdx 名敌人的全部落盘文件（新版两套动画 + 卡面 + 旧版单图）
static void DeleteEnemyFiles(int enemyIdx) {
    if (enemyIdx < 0 || enemyIdx >= kMaxBioSlots) return;
    for (int which = 0; which < 2; ++which)
        for (int f = 1; f <= EnemyFrameTotal(which); ++f)
            DeleteFileW(EnemyFilePath(enemyIdx, which, f).c_str());
    DeleteFileW(EnemyCardFilePath(enemyIdx).c_str());
    DeleteFileW(LegacyEnemyFilePath(enemyIdx + 1).c_str());
}

// 把第 from 名敌人的所有落盘文件整体改名成第 to 名（会覆盖 to 上的旧文件）
static void MoveEnemyFiles(int from, int to) {
    if (from < 0 || from >= kMaxBioSlots || to < 0 || to >= kMaxBioSlots) return;
    DeleteEnemyFiles(to);                                  // 目标编号先清干净
    for (int which = 0; which < 2; ++which) {
        const int total = EnemyFrameTotal(which);
        for (int f = 1; f <= total; ++f) {
            const std::wstring src = EnemyFilePath(from, which, f);
            if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            MoveFileExW(src.c_str(), EnemyFilePath(to, which, f).c_str(),
                        MOVEFILE_REPLACE_EXISTING);
        }
    }
    const std::wstring cardSrc = EnemyCardFilePath(from);
    if (GetFileAttributesW(cardSrc.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(cardSrc.c_str(), EnemyCardFilePath(to).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
    const std::wstring legacySrc = LegacyEnemyFilePath(from + 1);
    if (GetFileAttributesW(legacySrc.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(legacySrc.c_str(), LegacyEnemyFilePath(to + 1).c_str(),
                    MOVEFILE_REPLACE_EXISTING);
}

static void DeleteCustomCard(int group, int index) {
    if (group < 0 || group >= CARDS_GROUP_COUNT) return;
    if (index < 0 || index >= (int)g_cardsCustomPaths[group].size()) return;

    if (group == CARDS_GROUP_MY) {
        // 我方卡牌列表是按「有图的英雄」压缩过的，要还原成真实英雄编号
        int heroSeen = -1, heroIndex = -1;
        for (int h = 0; h < MAX_HEROES; ++h) {
            std::wstring path;
            if (!HeroCardPathOnDisk(h, path) && !HeroFramePath(h, 0, 1, path) &&
                !HeroFramePath(h, 1, 1, path))
                continue;
            if (++heroSeen == index) { heroIndex = h; break; }
        }
        if (heroIndex < 0) return;
        DeleteHeroCard(heroIndex);
        LoadHeroFromDisk();                            // 同步设计弹窗里的英雄数据
        LoadCardsForDisplay();
        g_cardsHoverSlot = -1;
        g_cardsTip = L"已删除英雄 #" + std::to_wstring(heroIndex + 1) +
                     L" 的全部素材（重新开始游戏后生效）";
        return;
    }

    // 敌人：列表是按「有素材的敌人」压缩过的，先还原成真实敌人编号，
    // 删掉这一名的全部素材（动画帧 + 卡面），再把后面编号的敌人整体前移补位。
    int enemySeen = -1, enemyIndex = -1;
    for (int n = 0; n < kMaxBioSlots; ++n) {
        std::wstring path;
        if (!EnemyCardPathOnDisk(n, path) && !EnemyFramePath(n, 0, 1, path) &&
            !EnemyFramePath(n, 1, 1, path))
            continue;
        if (++enemySeen == index) { enemyIndex = n; break; }
    }
    if (enemyIndex < 0) return;
    DeleteEnemyFiles(enemyIndex);
    for (int n = enemyIndex + 1; n < kMaxBioSlots; ++n) MoveEnemyFiles(n, n - 1);
    LoadEnemiesFromDisk();                                // 同步设计弹窗里的敌人数据
    LoadCardsForDisplay();
    g_cardsHoverSlot = -1;
    g_cardsTip = L"已删除敌人 #" + std::to_wstring(enemyIndex + 1) +
                 L" 的全部素材（动画帧 + 卡面，重新开始游戏后生效）";
}

// ---- 已有卡牌弹窗版式（逻辑坐标，复用 kPopupW / kPopupH）----
static RectF CardsBuiltinSlotRect(int index) {
    const float side = 80.f, gap = 28.f;
    const int cnt = (int)g_cardsBuiltIn.size();
    const float totalW = cnt * side + (cnt > 1 ? (cnt - 1) * gap : 0);
    return RectF(S((kPopupW - totalW) * 0.5f + index * (side + gap)),
                 S(92.f), S(side), S(side));
}
static RectF CardsTabRect(int index) {
    return RectF(S(36.f + index * 208.f), S(196.f), S(196.f), S(46.f));
}
static RectF CardsSlotRect(int index) {
    const float gridW = kBioColumns * kSlotSide + (kBioColumns - 1) * kSlotGap;
    const int col = index % kBioColumns;
    const int row = index / kBioColumns;
    return RectF(S((kPopupW - gridW) * 0.5f + col * (kSlotSide + kSlotGap)),
                 S(278.f + row * (kSlotSide + kSlotGap)),
                 S(kSlotSide), S(kSlotSide));
}
static RectF CardsSlotRemoveRect(const RectF& slot) {
    return RectF(slot.X + slot.Width - S(30.f), slot.Y, S(30.f), S(30.f));
}

static void DrawCardsBuiltinSlot(Graphics& g, const RectF& slot,
                                 Bitmap* image, const std::wstring& name) {
    GraphicsPath path;
    AddRoundRect(path, slot.X, slot.Y, slot.Width, slot.Height, S(10.f));
    SolidBrush fill(Color(255, 0x1b, 0x1b, 0x22));
    g.FillPath(&fill, &path);
    if (image) {
        const float scale = std::min((slot.Width - S(10.f)) / image->GetWidth(),
                                     (slot.Height - S(10.f)) / image->GetHeight());
        const float drawW = image->GetWidth() * scale, drawH = image->GetHeight() * scale;
        g.DrawImage(image, slot.X + (slot.Width - drawW) * 0.5f,
                            slot.Y + (slot.Height - drawH) * 0.5f, drawW, drawH);
    }
    Pen border(Color(255, 0x55, 0x55, 0x60), g_scale);
    g.DrawPath(&border, &path);
    DrawTextInRect(g, name, RectF(slot.X, slot.Y + slot.Height + S(2.f), slot.Width, S(20.f)),
                   g_fonts.footer.get(), Color(255, 0x96, 0x96, 0xa2));
}

static void DrawCardsSlot(Graphics& g, const RectF& slot, Bitmap* image, bool hovered) {
    GraphicsPath path;
    AddRoundRect(path, slot.X, slot.Y, slot.Width, slot.Height, S(12.f));
    if (image) {
        SolidBrush fill(Color(255, 0x1b, 0x1b, 0x22));
        g.FillPath(&fill, &path);
        const float scale = std::min((slot.Width - S(14.f)) / image->GetWidth(),
                                     (slot.Height - S(14.f)) / image->GetHeight());
        const float drawW = image->GetWidth() * scale, drawH = image->GetHeight() * scale;
        g.DrawImage(image, slot.X + (slot.Width - drawW) * 0.5f,
                            slot.Y + (slot.Height - drawH) * 0.5f, drawW, drawH);
        Pen border(hovered ? Color(255, 0xe0, 0x8a, 0x8a) : Color(255, 0x33, 0x33, 0x3d), g_scale);
        g.DrawPath(&border, &path);
        if (hovered) {
            const RectF badge = CardsSlotRemoveRect(slot);
            GraphicsPath bp;
            AddRoundRect(bp, badge.X, badge.Y, badge.Width, badge.Height, S(8.f));
            SolidBrush bf(Color(235, 0xc0, 0x50, 0x50));
            g.FillPath(&bf, &bp);
            DrawTextInRect(g, L"×", badge, g_fonts.status.get(), Color(255, 0xff, 0xff, 0xff));
        }
    } else {
        SolidBrush fill(Color(255, 0x12, 0x12, 0x16));
        g.FillPath(&fill, &path);
        Pen border(Color(40, 0x34, 0x34, 0x3e), g_scale);
        g.DrawPath(&border, &path);
    }
}

static void DrawCardsTab(Graphics& g, int index, bool active) {
    const RectF r = CardsTabRect(index);
    GraphicsPath path;
    AddRoundRect(path, r.X, r.Y, r.Width, r.Height, S(10.f));
    SolidBrush fill(active ? Color(255, 0x2b, 0x2b, 0x36) : Color(255, 0x1a, 0x1a, 0x21));
    g.FillPath(&fill, &path);
    Pen border(active ? Color(255, 0x86, 0xd9, 0x8a) : Color(255, 0x34, 0x34, 0x3e), g_scale);
    g.DrawPath(&border, &path);
    const int total = (index == CARDS_GROUP_MY) ? MAX_HEROES : kMaxBioSlots;
    std::wstring label = kCardsGroupNames[index];
    label += L"  " + std::to_wstring((int)g_cardsCustom[index].size()) + L"/" + std::to_wstring(total);
    DrawTextInRect(g, label, r, g_fonts.button.get(),
                   active ? Color(255, 0xf2, 0xf2, 0xf5) : Color(255, 0x96, 0x96, 0xa2));
}

static void DrawCardsPopupScene(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const float W = kPopupW * g_scale, H = kPopupH * g_scale;
    RectF full(0, 0, W, H);
    LinearGradientBrush bg(full, Color(255, 0x14, 0x14, 0x18),
                           Color(255, 0x1f, 0x1f, 0x26), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);
    // 标题
    DrawTextInRect(g, L"已有卡牌", RectF(S(36.f), S(22.f), S(300.f), S(52.f)),
                   g_fonts.title.get(), Color(255, 0xf2, 0xf2, 0xf5), StringAlignmentNear);
    DrawTextInRect(g, L"内置卡牌不可删除；我方 / 敌人卡牌悬停后点右上角 × 可删除",
                   RectF(S(220.f), S(44.f), W - S(300.f), S(24.f)),
                   g_fonts.footer.get(), Color(255, 0x78, 0x78, 0x82), StringAlignmentFar);
    { GraphicsPath bar; AddRoundRect(bar, S(36.f), S(78.f), S(48.f), S(3.f), S(1.5f));
      SolidBrush br(Color(255, 0x86, 0xd9, 0x8a)); g.FillPath(&br, &bar); }

    // 内置卡牌行
    if (g_cardsBuiltIn.empty()) {
        DrawTextInRect(g, L"（未找到内置卡牌图片）",
                       RectF(0, S(92.f), W, S(80.f)),
                       g_fonts.status.get(), Color(255, 0x78, 0x78, 0x82));
    } else {
        for (size_t i = 0; i < g_cardsBuiltIn.size(); ++i)
            DrawCardsBuiltinSlot(g, CardsBuiltinSlotRect((int)i),
                                 g_cardsBuiltIn[i].get(), g_cardsBuiltInNames[i]);
    }

    // 两组页签：我方卡牌 / 敌人卡牌
    for (int i = 0; i < CARDS_GROUP_COUNT; ++i) DrawCardsTab(g, i, i == g_cardsTab);
    DrawTextInRect(g, kCardsGroupHints[g_cardsTab],
                   RectF(S(36.f), S(254.f), W - S(72.f), S(20.f)),
                   g_fonts.status.get(), Color(255, 0x9a, 0x9a, 0xa6), StringAlignmentNear);

    // 当前组的卡牌网格（我方最多 13 个英雄，敌人最多 13 张）
    const int slots = (g_cardsTab == CARDS_GROUP_MY) ? MAX_HEROES : kMaxBioSlots;
    for (int i = 0; i < slots; ++i) {
        Bitmap* thumb = (i < (int)g_cardsCustom[g_cardsTab].size())
                        ? g_cardsCustom[g_cardsTab][i].get() : nullptr;
        DrawCardsSlot(g, CardsSlotRect(i), thumb, g_cardsHoverSlot == i);
    }
    // 我方卡牌：格子下方标出英雄编号，便于对应「设计角色」里的编号
    if (g_cardsTab == CARDS_GROUP_MY) {
        for (int i = 0; i < (int)g_cardsCustomNames[g_cardsTab].size(); ++i) {
            const RectF s = CardsSlotRect(i);
            DrawTextInRect(g, g_cardsCustomNames[g_cardsTab][i],
                           RectF(s.X, s.Y + s.Height - S(22.f), s.Width, S(20.f)),
                           g_fonts.footer.get(), Color(255, 0x9e, 0xe8, 0xa2));
        }
    }

    // 底部提示 + 返回按钮
    DrawTextInRect(g, g_cardsTip.empty()
                        ? L"提示：点「我方卡牌 / 敌人卡牌」切换；悬停图片后点右上角 × 可删除（内置卡牌不可删除）"
                        : g_cardsTip,
                   TipTextRect(), g_fonts.footer.get(), Color(255, 0x8a, 0x92, 0x9e),
                   StringAlignmentNear);
    DrawPopupButton(g, CloseButtonRect(), L"返回",
                    Color(255, 0x24, 0x24, 0x2c), Color(255, 0xb8, 0xb8, 0xc2));
}

// ---- 已有卡牌弹窗命中测试 ----
enum CardsTarget { CARDS_NONE = 0, CARDS_CLOSE, CARDS_TAB, CARDS_SLOT };
static CardsTarget HitTestCards(const PointF& p, int& index, bool& onRemove) {
    index = -1; onRemove = false;
    if (CloseButtonRect().Contains(p)) return CARDS_CLOSE;
    for (int i = 0; i < CARDS_GROUP_COUNT; ++i)
        if (CardsTabRect(i).Contains(p)) { index = i; return CARDS_TAB; }
    const int slots = (g_cardsTab == CARDS_GROUP_MY) ? MAX_HEROES : kMaxBioSlots;
    for (int i = 0; i < slots; ++i) {
        const RectF slot = CardsSlotRect(i);
        if (!slot.Contains(p)) continue;
        index = i;
        onRemove = (i < (int)g_cardsCustom[g_cardsTab].size()) &&
                   CardsSlotRemoveRect(slot).Contains(p);
        return CARDS_SLOT;
    }
    return CARDS_NONE;
}

static LRESULT CALLBACK CardsPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem) { EndPaint(hwnd, &ps); return 0; }
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        if (!bmp) { DeleteDC(mem); EndPaint(hwnd, &ps); return 0; }
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        { Graphics gg(mem); DrawCardsPopupScene(gg); }
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old) SelectObject(mem, old);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&track);
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1; bool onRemove = false;
        const CardsTarget target = HitTestCards(point, index, onRemove);
        const int hoverSlot = (target == CARDS_SLOT) ? index : -1;
        if (hoverSlot != g_cardsHoverSlot) {
            g_cardsHoverSlot = hoverSlot;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_cardsHoverSlot != -1) {
            g_cardsHoverSlot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const PointF point((REAL)(short)LOWORD(lp), (REAL)(short)HIWORD(lp));
        int index = -1; bool onRemove = false;
        switch (HitTestCards(point, index, onRemove)) {
        case CARDS_CLOSE:
            DestroyWindow(hwnd);
            break;
        case CARDS_TAB:
            if (index != g_cardsTab) {
                g_cardsTab = index;
                g_cardsHoverSlot = -1;
                g_cardsTip.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case CARDS_SLOT:
            if (index >= 0 && index < (int)g_cardsCustom[g_cardsTab].size() && onRemove) {
                const std::wstring what = (g_cardsTab == CARDS_GROUP_MY)
                    ? (L"这个英雄的全部素材（动画帧 / 卡面 / 子弹图）吗？")
                    : (L"这张敌人卡牌吗？");
                int ret = MessageBoxW(hwnd,
                    (L"确定删除" + what + L"删除后重新开始游戏生效。").c_str(),
                    L"删除确认", MB_YESNO | MB_ICONQUESTION);
                if (ret == IDYES) {
                    DeleteCustomCard(g_cardsTab, index);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            break;
        default: break;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT point; GetCursorPos(&point); ScreenToClient(hwnd, &point);
            const PointF cp((REAL)point.x, (REAL)point.y);
            int index = -1; bool onRemove = false;
            const CardsTarget target = HitTestCards(cp, index, onRemove);
            SetCursor(LoadCursor(nullptr, target != CARDS_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_cardsPopupHwnd == hwnd) g_cardsPopupHwnd = nullptr;
        if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowCharMenuWindow();
        else ShowLauncherWindow();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenCardsPopup() {
    if (g_cardsPopupHwnd && IsWindow(g_cardsPopupHwnd)) {
        ShowWindow(g_cardsPopupHwnd, SW_RESTORE);
        SetForegroundWindow(g_cardsPopupHwnd);
        return;
    }
    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = CardsPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kCardsPopupClass;
        RegisterClassExW(&wc);
        s_reg = true;
    }
    LoadCardsForDisplay();
    g_cardsTab = CARDS_GROUP_MY;   // 默认停「我方卡牌」，点页签可切到「敌人卡牌」
    g_cardsHoverSlot = -1;
    g_cardsTip.clear();

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{ 0, 0, (LONG)(kPopupW * g_scale), (LONG)(kPopupH * g_scale) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int wx, wy;
    HWND centerOn = (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ? g_charMenuHwnd : g_mainHwnd;
    if (centerOn && IsWindow(centerOn)) {
        RECT mw; GetWindowRect(centerOn, &mw);
        wx = mw.left + ((mw.right - mw.left) - ww) / 2;
        wy = mw.top + ((mw.bottom - mw.top) - wh) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        wx = wa.left + ((wa.right - wa.left) - ww) / 2;
        wy = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    }
    g_cardsPopupHwnd = CreateWindowExW(0, kCardsPopupClass, L"已有卡牌",
        style, wx, wy, ww, wh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_charMenuHwnd && IsWindow(g_charMenuHwnd)) ShowWindow(g_charMenuHwnd, SW_HIDE);
    else if (g_mainHwnd) ShowWindow(g_mainHwnd, SW_HIDE);

    if (g_cardsPopupHwnd) { ShowWindow(g_cardsPopupHwnd, SW_SHOW); UpdateWindow(g_cardsPopupHwnd); }
}

// ---------------------------------------------------------------------
// 命中测试 / 交互（按钮已是物理坐标，直接用客户区坐标）
// ---------------------------------------------------------------------
static PointF MouseInClient(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    return PointF((REAL)pt.x, (REAL)pt.y);
}

static int HitTest(const PointF& p) {
    for (size_t i = 0; i < g_buttons.size(); ++i)
        if (g_buttons[i].rect.Contains(p)) return (int)i;
    return -1;
}

static void RefreshHover(HWND hwnd) {
    PointF p = MouseInClient(hwnd);
    int hit = HitTest(p);
    bool changed = false;
    for (size_t i = 0; i < g_buttons.size(); ++i) {
        bool over = ((int)i == hit);
        if (g_buttons[i].over != over) { g_buttons[i].over = over; changed = true; }
    }
    if (changed) {
        InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
    }
}

static void SetActionStatus(const std::wstring& label, const std::wstring& fn) {
    g_status = L"已触发「" + label + L"」 → 接口位置 : " + fn + L"() （main.cpp 内 TODO 注释处）";
}

// ---------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_mainHwnd = hwnd;
        SetTimer(hwnd, 1, 16, nullptr);   // 悬停动画计时器
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // 双缓冲，避免闪烁；兼容位图创建失败时要安全退出，避免空句柄造成崩溃
        HDC mem = CreateCompatibleDC(hdc);
        if (!mem) {
            EndPaint(hwnd, &ps);
            return 0;
        }

        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        if (!bmp) {
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        {
            Graphics gg(mem);
            DrawScene(gg);
        }
        BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        if (old) SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                     // 背景在 WM_PAINT 中绘制
    case WM_TIMER:
        if (wp == 1) {                // 悬停动画插值
            bool dirty = false;
            for (auto& b : g_buttons) {
                float target = b.over ? 1.f : 0.f;
                if (std::fabs(b.hover - target) > 0.004f) {
                    b.hover += (target - b.hover) * 0.22f;
                    dirty = true;
                } else if (b.hover != target) {
                    b.hover = target;
                    dirty = true;
                }
            }

            if (g_mode == AppMode::Game) {
                //UpdateGame(1.f / 60.f);
                dirty = true;
            }

            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        RefreshHover(hwnd);
        if (GetCapture() == hwnd) {   // 拖拽按下状态
            PointF p = MouseInClient(hwnd);
            int hit = HitTest(p);
            for (auto& b : g_buttons)
                b.pressed = b.pressed && hit >= 0 && b.rect.Contains(p);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (GetCapture() == hwnd) {
            // 拖拽/按下状态由捕获循环继续控制，不能在离开窗口时直接取消按钮按下状态。
            return 0;
        }
        for (auto& b : g_buttons) { b.over = false; b.pressed = false; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        PointF p = MouseInClient(hwnd);
        if (g_mode == AppMode::Game) {
            const float left = S(104.f);
            const float top  = S(100.f);
            const float cellW = S(74.f);
            const float cellH = S(82.f);
            const int col = (int)((p.X - left) / cellW);
            const int row = (int)((p.Y - top) / cellH);
            if (p.X >= left && p.Y >= top && col >= 0 && row >= 0 && col < 9 && row < 5) {
                RectF tile(left + col * cellW + 4.f * g_scale,
                           top + row * cellH + 6.f * g_scale,
                           cellW - 12.f * g_scale, cellH - 16.f * g_scale);
                bool occupied = false;
                for (const auto& plant : g_plants) {
                    if (RectsOverlap(plant.rect, tile)) {
                        occupied = true;
                        break;
                    }
                }
                if (!occupied && g_sun >= 25) {
                    Plant plant;
                    plant.rect = tile;
                    plant.hp = 3;
                    g_plants.push_back(plant);
                    g_sun -= 25;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        int hit = HitTest(p);
        if (hit >= 0) {
            g_buttons[hit].pressed = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        PointF p = MouseInClient(hwnd);
        for (auto& b : g_buttons) {
            if (b.pressed && b.rect.Contains(p) && b.onClick) b.onClick();
            b.pressed = false;
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            PointF p = MouseInClient(hwnd);
            SetCursor(LoadCursor(nullptr, HitTest(p) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_mainHwnd == hwnd) g_mainHwnd = nullptr;
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------
// DPI 感知（Win10 1607+ 优先，老系统降级）
// ---------------------------------------------------------------------
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using PFN = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto p = (PFN)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

// ---------------------------------------------------------------------
// 入口：DPI 检测 → 字体/布局 → 创建窗口（尺寸一次到位，避免中途重排）
// ---------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    EnableDpiAwareness();
    SetAssetWorkingDirectory();

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);

    // DPI 缩放（主显示器）
    {
        HDC sdc = GetDC(nullptr);
        g_scale = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, sdc);
    }

    // 字体（字号 = 逻辑像素 × 缩放，绘制时零变换）
    g_family = new FontFamily(L"Microsoft YaHei UI");
    g_fonts.title  = std::make_unique<Font>(g_family, S(46.f), FontStyleBold,   UnitPixel);
    g_fonts.sub    = std::make_unique<Font>(g_family, S(12.f), FontStyleRegular,UnitPixel);
    g_fonts.button = std::make_unique<Font>(g_family, S(19.f), FontStyleRegular,UnitPixel);
    g_fonts.chev   = std::make_unique<Font>(g_family, S(17.f), FontStyleRegular,UnitPixel);
    g_fonts.status = std::make_unique<Font>(g_family, S(13.f), FontStyleRegular,UnitPixel);
    g_fonts.footer = std::make_unique<Font>(g_family, S(12.f), FontStyleRegular,UnitPixel);

    CreateButtons(g_scale);
    ResetHeroStats();     // 英雄数值（13 套）先给默认值，打开设计弹窗时再用 stats.ini 覆盖
    LoadDeckFromDisk();   // 读取出战卡组配置（deck.txt）到 g_deckTypes / g_deckCount / g_heroEnabled

    // 计算最终窗口矩形并居中（创建前一次到位）
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{ 0, 0, (LONG)(kDesignW * g_scale), (LONG)(kDesignH * g_scale) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int wx = wa.left + ((wa.right - wa.left) - ww) / 2;
    const int wy = wa.top  + ((wa.bottom - wa.top) - wh) / 2;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr;   // 自绘背景
    wc.lpszClassName = L"PvZLauncherWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"自定义塔防小游戏 · 启动器",
                                style, wx, wy, ww, wh,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { GdiplusShutdown(token); return 1; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 必须先于 GdiplusShutdown 释放所有 GDI+ 对象，且 Font 必须先于 FontFamily 释放
    g_fonts = {};
    delete g_family;
    g_family = nullptr;
    g_buttons.clear();

    // 「设计角色」弹窗里的缩略图（std::unique_ptr<Bitmap>）同样是带析构的静态对象，
    // 必须在这里一并释放：否则它们会等到进程退出（GdiplusShutdown 之后）才析构，
    // 在 GDI+ 已关闭的状态下销毁 Bitmap → 访问冲突崩溃
    // （调试器里堆栈会停在 std::default_delete<Gdiplus::Bitmap>::operator()）。
    g_importedEntityPreview.reset();
    // 敌人缩略图（13 名 × 行走 6 帧 + 攻击 8 帧 + 卡面）同样要在 GdiplusShutdown 前释放
    for (int i = 0; i < kMaxBioSlots; ++i) {
        g_enemies[i].walkThumbs.clear();
        g_enemies[i].attackThumbs.clear();
        g_enemies[i].cardThumb.reset();
    }
    // 英雄缩略图（13 个英雄 × 两套 8 帧 + 卡面 + 子弹图）同样要在 GdiplusShutdown 前释放
    for (int i = 0; i < MAX_HEROES; ++i) {
        g_heroes[i].attackThumbs.clear();
        g_heroes[i].idleThumbs.clear();
        g_heroes[i].cardThumb.reset();
        g_heroes[i].bulletThumb.reset();
    }

    // 「已有卡牌」弹窗里的缩略图（同样必须在 GdiplusShutdown 前释放，否则进程退出崩溃）
    g_cardsBuiltIn.clear();
    g_cardsBuiltInNames.clear();
    for (int i = 0; i < CARDS_GROUP_COUNT; ++i) {
        g_cardsCustom[i].clear();
        g_cardsCustomPaths[i].clear();
    }

    GdiplusShutdown(token);
    return (int)msg.wParam;
}

#include "game.cpp"

bool SelectEntityImages(std::vector<std::wstring>& outPaths,
                        const wchar_t* title, int maxCount) {
    outPaths.clear();
    if (maxCount <= 0) return false;

    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
        | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    dialog.lpstrTitle = (title && *title) ? title : L"选择图片";

    if (!GetOpenFileNameW(&dialog)) return false;

    const wchar_t* cursor = buffer.data();
    const std::wstring first(cursor);
    cursor += first.size() + 1;
    if (*cursor == L'\0') {
        outPaths.push_back(first);
        return true;
    }

    std::wstring directory = first;
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
        directory += L'\\';

    while (*cursor != L'\0' && outPaths.size() < static_cast<size_t>(maxCount)) {
        const std::wstring name(cursor);
        cursor += name.size() + 1;
        if (!name.empty()) outPaths.push_back(directory + name);
    }
    return !outPaths.empty();
}
