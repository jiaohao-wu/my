// 开发日志
//1导入素材
//2实现最开始的游戏场景
//3实现游戏顶部的工具栏
//4实现工具栏里面的游戏卡牌
#define WIN_WIDTH 960
#define WIN_HEIGHT 640
#include <atomic>
#include <cstdlib>
#include <cstring>
//定义tower类型
enum { WAN_DOU, XIANG_RI_KUI, USER_ENTITY, towerCOUNT };
#include<stdio.h>
#include<graphics.h>//easyx图形库的头文件
#include"vector2.h"//向量引用
#include<time.h>
#include<math.h>
#include<mmsystem.h>//导入音乐收集能量的时候
//导入一个库
#include<Windows.h>
#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"EasyXw.lib")

std::atomic_bool g_gameRunning = false;

// =====================================================================
// 胜负机制（需求：能量累计收集到 10000 判胜；中途退出 / 被突破判负）
//   注意：sunshine 是「当前余额」，放卡会花掉、来回增减，不适合当胜利目标，
//         所以另设 g_sunshineTotal 记「本局累计收集量」，只增不减。
// =====================================================================
const int SUNSHINE_WIN_TOTAL = 10000;   // 累计收集到这么多能量即判定胜利
int g_sunshineTotal = 0;                // 本局累计收集的能量（g_sunshineTotal，非余额）

// 本局结束原因：主循环退出后由 GameMain 统一弹提示，不在游戏体内直接弹窗
enum GameResult {
	GAME_RESULT_NONE = 0,       // 未结束（占位）
	GAME_RESULT_WIN,            // 累计能量达标 → 胜利
	GAME_RESULT_LOSE_EXIT,      // 在游戏体内点「退出游戏」→ 直接判负
	GAME_RESULT_LOSE_BREACH     // 敌人走到最左侧突破防线 → 判负
};
GameResult g_gameResult = GAME_RESULT_NONE;

IMAGE imgBg;//全局变量表示背景图片
IMAGE imgBar;//工具栏
IMAGE imgCards[towerCOUNT];
IMAGE* imgtower[towerCOUNT][20];
IMAGE imgUserEntity;
bool userEntityLoaded = false;
int curX, curY;//当前选中的tower，在拖动过程中的位置
int curtower;//0没有选中。 1选中了第一种tower

// =====================================================================
// 自定义敌人（由启动器「设计角色 → 敌人生物」写入 img/custom/*.png，游戏启动时读入）
//   最多 13 名敌人，每名敌人两套序列帧：
//     行走动画 enemy{N}_walk_1..6.png     （6 张）
//     攻击动画 enemy{N}_attack_1..8.png   （8 张）
//   N = 敌人编号 1..13，两边改要一起改（main.cpp 的 EnemyFilePath）。
//   旧版只有一张静态图 enemy_N.png → 只读兼容，当作「行走动画第 1 帧」。
// （防御生物 / 攻击生物已移除：旧的 defense_*.png、attack_*.png 留在磁盘但不再读取）
// =====================================================================
const int CUSTOM_MAX = 13;          // 最多 13 名敌人（与 main.cpp 的 kMaxBioSlots 一致）
// 注意：ENEMY_WALK_FRAMES / ENEMY_ATTACK_FRAMES 定义在 main.cpp（UI 需要），
//       本文件被 #include 进 main.cpp，故可见。
enum {
	CARD_PER_PAGE = 8               // 工具栏每页显示几张卡
};

// 一名敌人的两套动画
struct EnemyArt {
	IMAGE* walk[ENEMY_WALK_FRAMES];
	IMAGE* attack[ENEMY_ATTACK_FRAMES];
	int    walkCount;
	int    attackCount;
	bool   loaded;
};
EnemyArt g_enemyArt[CUSTOM_MAX];
int customEnemyCount = 0;   // 已配置成功的敌人数（按编号独立配置，空号跳过）
// 已配置的敌人编号列表（0-based，值域 0..CUSTOM_MAX-1）。
// 用户可以跳过某个编号直接配后面的，所以 creatZM() 不能再用 1+rand()%customEnemyCount
// 这种「连续编号」假设去挑，必须从这个列表里随机选，否则会挑到空号 → 敌人刷不出来。
int g_enemySpawnList[CUSTOM_MAX];
int g_enemySpawnCount = 0;

// =====================================================================
// 英雄卡牌阵营（最多 13 个英雄，每个英雄一套图 + 一套数值）
//   每个英雄由用户上传两套各 8 张图片：
//     攻击状态 heroN_attack_1..8 / 休闲状态 heroN_idle_1..8（N = 英雄编号 1..13）
//   另可配 1 张卡面 heroN_card.png（工具栏图标，可选）。
//   两种状态都是 8 帧动画；英雄只能近身（近战）攻击、可以移动位置。
//   数值（血量 / 费用 / 伤害）在「设计角色 → 英雄」里按英雄分别配置，
//   存于 stats.ini 的 [hero] 段（1..13 行）。
// =====================================================================
const int HERO_TYPE        = 300;   // 英雄卡 type 基址：第 n 个英雄 = HERO_TYPE + n
// 注意：HERO_FRAMES / MAX_HEROES 定义在 main.cpp（UI 需要），
//       本文件被 #include 进 main.cpp，故可见。
const int HERO_W           = 72;    // 英雄贴图统一适配宽
const int HERO_H           = 96;    // 英雄贴图统一适配高
const int HERO_MELEE_RANGE = 72;    // 近战命中范围（像素，英雄中心 ± 该值）
const int HERO_ATK_INTERVAL = 16;   // 攻击节奏：每多少帧造成一次伤害（约 0.32s @20ms）

// 一个英雄的两套 8 帧动画 + 卡面 + 子弹图
struct HeroArt {
	IMAGE* attack[HERO_FRAMES] = { nullptr };
	IMAGE* idle[HERO_FRAMES]   = { nullptr };
	int    attackCount = 0;
	int    idleCount   = 0;
	IMAGE  card;                    // 英雄卡面（工具栏图标），可选
	bool   cardLoaded = false;
	bool   loaded     = false;      // 至少传了一套（攻击或休闲）图，这个英雄才可用
	IMAGE* bullet     = nullptr;    // 子弹图（可选）：配了 = 远程英雄，没配 = 近战英雄
	bool   bulletLoaded = false;
};
HeroArt g_heroArt[MAX_HEROES];

// 远程英雄的子弹：贴图统一适配尺寸 + 射击节奏
const int HERO_BULLET_W     = 44;   // 英雄子弹贴图统一适配宽
const int HERO_BULLET_H     = 32;   // 英雄子弹贴图统一适配高
const int HERO_BULLET_SPEED = 6;    // 远程英雄的子弹飞行速度（像素/帧）
const int HERO_SHOOT_INTERVAL = 20; // 远程英雄射击节奏：每多少帧发一发（约 0.4s @20ms）
const int HERO_RANGE_RANGE  = 520;  // 远程英雄的射程（像素，英雄中心 ± 该值）

// 第 h 个英雄是不是远程英雄（配了子弹图就是远程，没配就是近战）
static bool heroIsRanged(int h) {
	return h >= 0 && h < MAX_HEROES && g_heroArt[h].bulletLoaded;
}

// ---- 出战卡组 / 英雄携带：由启动器「出战卡组」弹窗写入，game.cpp 只读 ----
extern int  g_deckTypes[8];         // 用户选中的 8 张卡对应的 type（顺序即卡序）
extern int  g_deckCount;            // 实际选中数量（0 = 未配置，回退为「全部可用」）
extern bool g_heroEnabled;          // 是否携带英雄
extern BioStat g_heroStat[MAX_HEROES];  // 每个英雄一套数值：{hp, cost, dmg}

struct Hero {
	bool placed = false;     // 是否在场上
	bool held   = false;     // 是否正被拿起移动中
	int  cellRow = 0, cellCol = 0;   // 所在草坪格（3 行 9 列）
	int  x = 0, y = 0;      // 当前像素左上角（跟随光标时实时更新）
	int  state  = 0;        // 0 = 休闲(idle)，1 = 攻击(attack)
	int  frameIndex = 0;    // 当前帧 0..HERO_FRAMES-1
	int  frameTimer = 0;    // 帧推进计时
	int  atkTimer   = 0;    // 近战造成伤害的节奏计时
	int  hp = 0, cost = 0, dmg = 0;
	int  kind = 0;          // 对应第几个英雄（0..MAX_HEROES-1）
};
Hero g_heroUnits[MAX_HEROES];       // 每个英雄最多上场一个，因此最多 13 个英雄同时在场
                                    // （注意：main.cpp 里的 g_heroes 是 UI 侧的英雄编辑数据，别混淆）
int  g_heroHeldIndex = -1;          // 当前被拿起移动的英雄下标（-1 = 没有）

// 英雄卡 type → 英雄下标（0..MAX_HEROES-1）；不是英雄卡返回 false
static bool heroTypeIndex(int type, int& index) {
	if (type > HERO_TYPE && type <= HERO_TYPE + MAX_HEROES) {
		index = type - HERO_TYPE - 1;
		return true;
	}
	return false;
}

// 英雄卡面图：优先用「英雄卡面」（用户单独上传的图标），否则退而用动画首帧
IMAGE* heroFaceImage(int h) {
	if (h < 0 || h >= MAX_HEROES) return nullptr;
	HeroArt& art = g_heroArt[h];
	if (art.cardLoaded) return &art.card;
	if (art.idleCount > 0 && art.idle[0]) return art.idle[0];
	if (art.attackCount > 0 && art.attack[0]) return art.attack[0];
	return nullptr;
}


// =====================================================================
// 经济系统：放置卡牌要消耗能量
//   能量来源：每 2 秒自动到账（基础 10 + 每个收集者 10），见 autoSunshineIncome()
//   能量去处：每放下一株生物，立刻扣掉这张卡的 cost
//   默认每张卡 100 能量；想给某类卡单独定价，改 cardCostForType() 一行即可。
// =====================================================================
const int DEFAULT_CARD_COST = 100;  // 卡牌默认能量消耗

// 工具栏卡槽：内置卡牌 + 自定义防御/攻击生物，排成一条可翻页的卡槽
struct CardSlot {
	int type;                       // 放置时写入 map[row][col].type 的值
	IMAGE* image;                   // 卡面图
	int cost;                       // 放置这张卡要花的能量
};
const int CARD_MAX = towerCOUNT + MAX_HEROES + 8;  // 内置卡 + 英雄卡（最多 13 张）+ 余量
CardSlot cards[CARD_MAX];
int cardCount = 0;                  // 实际卡槽数量
int cardPage = 0;                   // 当前页（从 0 开始）

// 载入PNG图并去透明部分
void _putimagePNG(int  picture_x, int picture_y, IMAGE* picture) //x为载入图片的X坐标，y为Y坐标
{
	DWORD* dst = GetImageBuffer();    // GetImageBuffer()函数，用于获取绘图设备的显存指针，EASYX自带
	DWORD* draw = GetImageBuffer();
	DWORD* src = GetImageBuffer(picture); //获取picture的显存指针
	int picture_width = picture->getwidth(); //获取picture的宽度，EASYX自带
	int picture_height = picture->getheight(); //获取picture的高度，EASYX自带
	int graphWidth = getwidth();       //获取绘图区的宽度，EASYX自带
	int graphHeight = getheight();     //获取绘图区的高度，EASYX自带
	int dstX = 0;    //在显存里像素的角标

	// 实现透明贴图 公式： Cp=αp*FP+(1-αp)*BP ， 贝叶斯定理来进行点颜色的概率计算
	for (int iy = 0; iy < picture_height; iy++)
	{
		for (int ix = 0; ix < picture_width; ix++)
		{
			int srcX = ix + iy * picture_width; //在显存里像素的角标
			int sa = ((src[srcX] & 0xff000000) >> 24); //0xAArrggbb;AA是透明度
			int sr = ((src[srcX] & 0xff0000) >> 16); //获取RGB里的R
			int sg = ((src[srcX] & 0xff00) >> 8);   //G
			int sb = src[srcX] & 0xff;              //B
			if (ix >= 0 && ix <= graphWidth && iy >= 0 && iy <= graphHeight && dstX <= graphWidth * graphHeight)
			{
				dstX = (ix + picture_x) + (iy + picture_y) * graphWidth; //在显存里像素的角标
				int dr = ((dst[dstX] & 0xff0000) >> 16);
				int dg = ((dst[dstX] & 0xff00) >> 8);
				int db = dst[dstX] & 0xff;
				draw[dstX] = ((sr * sa / 255 + dr * (255 - sa) / 255) << 16)
					| ((sg * sa / 255 + dg * (255 - sa) / 255) << 8)
					| (sb * sa / 255 + db * (255 - sa) / 255);
			}
		}
	}
}

// 适用于 y <0 以及x<0的任何情况
void putimagePNG(int x, int y, IMAGE* picture) {

	IMAGE imgTmp, imgTmp2, imgTmp3;
	int winWidth = getwidth();
	int winHeight = getheight();
	if (y < 0) {
		SetWorkingImage(picture);
		getimage(&imgTmp, 0, -y,
			picture->getwidth(), picture->getheight() + y);
		SetWorkingImage();
		y = 0;
		picture = &imgTmp;
	}
	else if (y >= getheight() || x >= getwidth()) {
		return;
	}
	else if (y + picture->getheight() > winHeight) {
		// 底部超出窗口：截取图片"顶部"能显示的那一段。
		// 注意 getimage 的第 2、3 个参数是"源图左上角坐标"，必须传 0,0；
		// 传成目标坐标 x,y 会截错区域甚至取到图外，导致花屏。
		SetWorkingImage(picture);
		getimage(&imgTmp, 0, 0, picture->getwidth(), winHeight - y);
		SetWorkingImage();
		picture = &imgTmp;
	}

	if (x < 0) {
		SetWorkingImage(picture);
		getimage(&imgTmp2, -x, 0, picture->getwidth() + x, picture->getheight());
		SetWorkingImage();
		x = 0;
		picture = &imgTmp2;
	}

	if (x > winWidth - picture->getwidth()) {
		SetWorkingImage(picture);
		getimage(&imgTmp3, 0, 0, winWidth - x, picture->getheight());
		SetWorkingImage();
		picture = &imgTmp3;
	}


	_putimagePNG(x, y, picture);
}

int getDelay() {
	static unsigned long long lastTime = 0;
	unsigned long long currentTime = GetTickCount();
	if (lastTime == 0) {
		lastTime = currentTime;
		return 0;
	}
	else {
		int ret = currentTime - lastTime;
		lastTime = currentTime;
		return ret;
	}
}

struct tower
{
	int type;//种类 0：表示没有 1：第一种
	int frameIndex;//序列帧的序号

	bool catched;//是否被敌人捕获
	int hp;//血量：被啃食扣血，<=0 就消失（数值来自「设计角色」里配的血量）

	int x, y;
	int timer;
};

struct tower map[3][9];

const wchar_t* GetImportedEntityPath();
//定义一个能量结构体

enum { SUNSHINE_DOWN, SUNSHINE_GROUND, SUNSHINE_COLLECT, SUNSHINE_RPODUCT };

struct sunshineBall
{
	int x, y;//能量球飘落位置的坐标（x不变）
	int frameIndex;//当前图片显示帧的序号
	//能量有一个落点设置
	int destY;//飘落位置的Y坐标
	bool used;//判断是否在使用
	//计时器
	int timer;

	float xoff;
	float yoff;

	float t;//贝塞尔曲线时间点0..1
	vector2 p1, p2, p3, p4;//分别对应起点终点控制点
	vector2 pCur;//当前时刻能量球的位置
	float speed;
	int status;//能量状态
};

struct sunshineBall balls[10];//设置10个能量池（已不再用于掉落/收集玩法，结构体保留供扩展）
IMAGE imgSunshineBall[29];//加载能量图片，一共29放入数组中
int sunshine;

// ---- 自动能量收入规则 ----
// 不再从天上掉能量、也不用点击收集：每 2 秒自动结算一次。
// 基础收入 10；每在场上放置一个"产能量"生物（收集者），每 2 秒再多 10。
const int SUNSHINE_BASE_INCOME        = 10;    // 每 2 秒的基础能量
const int SUNSHINE_PER_PRODUCER       = 10;    // 每个产能量生物每 2 秒的额外能量
const int SUNSHINE_INCOME_INTERVAL_MS = 2000;  // 结算间隔（毫秒）
// 开局能量：至少够放下第一张卡（默认 100），否则开局只能干等自动收入
const int SUNSHINE_START              = 150;

// 前向声明（定义在"能量"相关的函数区，供 HUD 提前调用）
int countSunProducers();   // 场上产能量生物（向日葵）的数量
int sunIncomePer2s();      // 每 2 秒自动获得的能量总量

DWORD g_sunIncomeLastTick = 0;   // 上次结算能量的时刻（gameInit 里重置，避免重开时补发）

struct zm
{
	int x, y;
	int row;
	int frameIndex;
	bool used;
	int speed;
	int blood;//僵尸血条
	bool dead;
	bool eating;//正在攻击
	int customType;//0=默认僵尸；n>0 → 使用 imgCustomEnemy[n-1] 这张自定义敌人图
	int dmg;//每次啃食造成的伤害（默认僵尸 1，自定义敌人用「设计角色」里配的伤害）
};
struct zm zms[10];
IMAGE imgZM[22];
IMAGE imgZMDead[20];
IMAGE imgZMEat[21];

//子弹的数据类型
struct bullet
{
	int x, y;
	int row;
	bool used;
	int speed;
	bool blast;//定义tower射出的子弹是否发生爆炸
	int frameIndex;//帧序号
	int damage;//命中敌人时扣多少血（豌豆固定值；远程英雄用该英雄配的伤害）
	int bulletKind;//0=游戏默认子弹；301..313 = 第 n 个远程英雄自己的子弹图
};

struct bullet bullets[30];
IMAGE imgBulletNormal;
IMAGE imgBallBlast[4];

bool fileExist(const wchar_t* name)
{
	FILE* fp = nullptr;
	_wfopen_s(&fp, name, L"r");//r表示文件的读取
	if (fp == NULL)
	{
		return false;
	}
	else
	{
		fclose(fp);
		return true;
	}
}

// 将导入图片的白色背景转换为透明像素，供 _putimagePNG 使用。
// 对接近白色的边缘保留部分透明度，避免缩放后的白边过于明显。
void removeWhiteBackground(IMAGE* image)
{
	if (image == nullptr || image->getwidth() <= 0 || image->getheight() <= 0) return;

	DWORD* pixels = GetImageBuffer(image);
	const int pixelCount = image->getwidth() * image->getheight();
	for (int i = 0; i < pixelCount; i++)
	{
		const int sourceAlpha = (pixels[i] >> 24) & 0xff;
		const int red = (pixels[i] >> 16) & 0xff;
		const int green = (pixels[i] >> 8) & 0xff;
		const int blue = pixels[i] & 0xff;
		const int whiteness = std::min(red, std::min(green, blue));

		if (whiteness >= 245)
		{
			pixels[i] &= 0x00ffffff;
		}
		else if (whiteness >= 200)
		{
			// 越接近白色越应该透明；保留原图 alpha，避免把透明边缘重新刷实。
			const int backgroundAlpha = (245 - whiteness) * 255 / 45;
			const int alpha = sourceAlpha * backgroundAlpha / 255;
			pixels[i] = (pixels[i] & 0x00ffffff) | (alpha << 24);
		}
	}
}

// ---------------------------------------------------------------------
// 自定义敌人：载入 / 卡槽组装 / 查询
// ---------------------------------------------------------------------

// 取第 enemyIdx 名敌人（1-based）某一帧在磁盘上的真实路径；文件不存在返回 false
//   which: 0 = 行走动画（6 帧）；1 = 攻击动画（8 帧）
//   旧版只上传了一张静态图 enemy_N.png → 当作「行走动画第 1 帧」回退，避免老素材失效
static bool enemyFramePath(int enemyIdx, int which, int frame, wchar_t* out)
{
	const wchar_t* kind = (which == 0) ? L"walk" : L"attack";
	swprintf_s(out, MAX_PATH, L"./img/custom/enemy%d_%s_%d.png", enemyIdx, kind, frame);
	if (fileExist(out)) return true;
	if (which == 0 && frame == 1)
	{
		swprintf_s(out, MAX_PATH, L"./img/custom/enemy_%d.png", enemyIdx);
		return fileExist(out);
	}
	return false;
}

// 载入一名敌人的一套动画（缺号即停止），返回实际载入帧数
// width/height 是统一适配的贴图尺寸（EasyX 会等比缩放并保留透明）
static int loadEnemyFrames(int enemyIdx, int which, IMAGE** slots, int maxFrames,
                           int width, int height)
{
	int count = 0;
	wchar_t path[MAX_PATH];
	for (int f = 1; f <= maxFrames; f++)
	{
		if (!enemyFramePath(enemyIdx, which, f, path)) break;

		IMAGE* image = new IMAGE;
		loadimage(image, path, width, height, true);
		if (image->getwidth() <= 0 || image->getheight() <= 0)
		{
			delete image;
			break;
		}
		removeWhiteBackground(image);
		slots[count] = image;
		count++;
	}
	return count;
}

void loadCustomBiologys()
{
	// 敌人：扫描 enemy1..13，每名敌人两套动画（行走 6 帧 / 攻击 8 帧）。
	// 注意：这里【不能】遇空号就 break —— 敌人是按编号独立配置的，用户完全可能
	// 跳过 1 号直接配 3 号（或中途删掉某一个），一旦 break 后面全加载不到，
	// 表现就是「明明添加了敌人，游戏体里却一个都不出现」。空号直接跳过即可。
	customEnemyCount = 0;
	g_enemySpawnCount = 0;
	for (int n = 0; n < CUSTOM_MAX; n++)
	{
		EnemyArt& art = g_enemyArt[n];
		for (int f = 0; f < ENEMY_WALK_FRAMES; f++)   art.walk[f] = NULL;
		for (int f = 0; f < ENEMY_ATTACK_FRAMES; f++)  art.attack[f] = NULL;
		art.walkCount = 0;
		art.attackCount = 0;
		art.loaded = false;

		const int wc = loadEnemyFrames(n + 1, 0, art.walk,   ENEMY_WALK_FRAMES,   92, 118);
		const int ac = loadEnemyFrames(n + 1, 1, art.attack, ENEMY_ATTACK_FRAMES, 92, 118);
		if (wc == 0 && ac == 0) continue;           // 这一号没配 → 跳过，继续看下一号

		art.walkCount = wc;
		art.attackCount = ac;
		art.loaded = true;
		customEnemyCount++;
		g_enemySpawnList[g_enemySpawnCount++] = n;
	}

	// 读「设计角色」里配的血量 / 费用 / 伤害（stats.ini）；没有就用默认值
	ResetBioStats();
	LoadBioStats();
}

// ---------------------------------------------------------------------
// 英雄卡牌：载入攻击 / 休闲两套各 8 张图（缺号即停止），并读入英雄数值
// ---------------------------------------------------------------------
// 取英雄素材在磁盘上的真实路径：先试新版 heroN_*.png，
// 1 号英雄再回退旧版 hero_*.png（与 main.cpp 的 HeroFramePath / HeroCardPathOnDisk 保持一致，
// 否则启动器认得老文件、游戏却加载不到，勾了卡组也出不来卡）。
static bool HeroArtPath(const wchar_t* pattern, int heroIdx, int frame, wchar_t* out)
{
	swprintf_s(out, MAX_PATH, pattern, heroIdx, frame);
	if (fileExist(out)) return true;
	if (heroIdx == 1)   // 旧版没有编号，等价于 1 号英雄
	{
		// pattern 形如 "./img/custom/hero%d_attack_%d.png" → 删掉 "%d" 两个字符
		// 得到 "./img/custom/hero_attack_%d.png"，再只填帧号。
		wchar_t legacy[MAX_PATH];
		wcscpy_s(legacy, pattern);
		wchar_t* p = wcsstr(legacy, L"hero%d");
		if (p) memmove(p + 4, p + 6, (wcslen(p + 6) + 1) * sizeof(wchar_t));
		swprintf_s(out, MAX_PATH, legacy, frame);
		return fileExist(out);
	}
	return false;
}

// 载入一套 8 帧动画（缺号即停止）；返回实际载入帧数
static int loadHeroFrames(const wchar_t* pattern, int heroIdx, IMAGE** slots)
{
	int count = 0;
	wchar_t name[MAX_PATH];
	for (int f = 1; f <= HERO_FRAMES; f++)
	{
		if (!HeroArtPath(pattern, heroIdx, f, name)) break;
		IMAGE* image = new IMAGE;
		loadimage(image, name, HERO_W, HERO_H, true);
		if (image->getwidth() <= 0 || image->getheight() <= 0) { delete image; break; }
		removeWhiteBackground(image);
		slots[count++] = image;
	}
	return count;
}

void loadHero()
{
	for (int h = 0; h < MAX_HEROES; h++)
	{
		HeroArt& art = g_heroArt[h];
		art.attackCount = loadHeroFrames(L"./img/custom/hero%d_attack_%d.png", h + 1, art.attack);
		art.idleCount   = loadHeroFrames(L"./img/custom/hero%d_idle_%d.png",   h + 1, art.idle);

		// 至少传了任意一套图，这个英雄才可用；两套都没传则完全不出现
		art.loaded = (art.attackCount > 0 || art.idleCount > 0);

		// 卡面（工具栏图标）：可选；存在则载入，并去白底
		art.cardLoaded = false;
		wchar_t path[MAX_PATH];
		if (HeroArtPath(L"./img/custom/hero%d_card.png", h + 1, 0, path))
		{
			loadimage(&art.card, path, HERO_W, HERO_H, true);
			if (art.card.getwidth() > 0 && art.card.getheight() > 0)
			{
				removeWhiteBackground(&art.card);
				art.cardLoaded = true;
			}
		}

		// 子弹图（远程英雄）：可选；配了就走远程射击，没配就走近战
		if (art.bullet == NULL) art.bullet = new IMAGE;
		art.bulletLoaded = false;
		if (HeroArtPath(L"./img/custom/hero%d_bullet.png", h + 1, 0, path))
		{
			loadimage(art.bullet, path, HERO_BULLET_W, HERO_BULLET_H, true);
			if (art.bullet->getwidth() > 0 && art.bullet->getheight() > 0)
			{
				removeWhiteBackground(art.bullet);
				art.bulletLoaded = true;
			}
		}

		// 数值（血量 / 费用 / 伤害）来自 stats.ini 的 [hero] 段（见 LoadBioStats）
		g_heroUnits[h].kind = h;
		g_heroUnits[h].cost = g_heroStat[h].cost;
		g_heroUnits[h].dmg  = g_heroStat[h].dmg;
	}
}


// ---- 自定义生物数值（血量 / 费用 / 伤害）----
// 数据本体在 main.cpp（g_bioStats），弹窗编辑后写进 img\custom\stats.ini，
// 这里只是按 type 取用；stats.ini 不存在就用默认值。
const int DEFAULT_PLANT_HP   = 100;  // 内置生物（豌豆/向日葵）的血量
const int DEFAULT_ZOMBIE_HP  = 100;  // 内置敌人的血量
const int DEFAULT_ZOMBIE_DMG = 1;    // 内置敌人每次啃食的伤害（保持原来的啃食时长）
const int DEFAULT_BULLET_DMG = 20;   // 豌豆每发子弹的伤害（保持原值）

// 每张卡能量定价：英雄用各自在「设计角色 → 英雄」里配的费用，内置生物统一默认价。
// 要单独给内置卡定价就在这里按 type 返回，例如让产能量的向日葵便宜些：
//   if (type == XIANG_RI_KUI + 1) return 50;
int cardCostForType(int type)
{
	int idx = 0;
	if (heroTypeIndex(type, idx)) return g_heroStat[idx].cost;   // 英雄卡各自定价
	return DEFAULT_CARD_COST;
}

// 任意 type → 卡面图（内置用专用卡牌图，英雄用各自卡面/首帧，内置序列帧生物用第 0 帧）
IMAGE* cardFaceForType(int type)
{
	int idx = 0;
	if (heroTypeIndex(type, idx)) return heroFaceImage(idx);
	if (type == USER_ENTITY + 1)  return userEntityLoaded ? &imgUserEntity : NULL;
	const int ti = type - 1;
	if (ti >= 0 && ti < towerCOUNT) return &imgCards[ti];
	return NULL;
}

// 把一个 type 加入工具栏卡槽（仅当卡面图可用，避免空卡；重复 type 自动跳过）
static void pushCard(int type)
{
	if (cardCount >= CARD_MAX) return;
	for (int i = 0; i < cardCount; i++)
		if (cards[i].type == type) return;   // 已经在卡槽里了（例如卡组里勾了英雄、开关也开了）
	IMAGE* img = cardFaceForType(type);
	if (img == NULL) return;
	cards[cardCount].type  = type;
	cards[cardCount].image = img;
	cards[cardCount].cost  = cardCostForType(type);
	cardCount++;
}

// 组装工具栏卡槽
//   1) 用户在「出战卡组」里选了卡（g_deckCount>0）→ 严格按勾选到的那几张出牌，多一张都不加；
//   2) 没配卡组时回退为「全部可用」，保持改动前的行为（向后兼容）；
//   3) 只有在没配卡组时，「携带英雄」开关才把配好图的英雄整批追加进去。
void buildCardList()
{
	cardCount = 0;
	if (g_deckCount > 0)
	{
		for (int i = 0; i < g_deckCount && i < 8; i++)
			pushCard(g_deckTypes[i]);
	}
	else
	{
		for (int i = 0; i < towerCOUNT; i++)
		{
			if (i == USER_ENTITY && !userEntityLoaded) continue;//没导入过就不占卡位
			pushCard(i + 1);
		}
	}

	// 英雄卡：只有在「用户没配卡组」时才按开关整批追加。
	// 一旦用户在出战卡组里勾了卡，就以勾选结果为准 —— 否则开关会把没勾选的英雄也塞进卡槽。
	if (g_deckCount == 0 && g_heroEnabled)
	{
		for (int h = 0; h < MAX_HEROES; h++)
		{
			if (g_heroArt[h].loaded || g_heroArt[h].cardLoaded)
				pushCard(HERO_TYPE + h + 1);
		}
	}

	cardPage = 0;
}

int cardPageCount()
{
	if (cardCount <= 0) return 1;
	return (cardCount + CARD_PER_PAGE - 1) / CARD_PER_PAGE;
}

// 枚举当前所有「可用」卡牌，供启动器「出战卡组」弹窗勾选。
//   内置植物：扫描 img/Cards/card_*.png（与游戏载入循环一致，缺号即停）；
//   导入角色：仅当已导入（GetImportedEntityPath 非空）才出现；
//   英雄：扫描 img/custom/heroN_*.png（N = 1..13），每个配好图的英雄一张卡。
// 只依赖磁盘，不依赖游戏运行时已载入的 IMAGE*，因此可在启动器阶段调用。
// 返回写入 out 的卡牌数量；每张给出 type / 名称 / 费用 / 卡面路径。
int CollectDeckCardInfos(DeckCardInfo* out, int maxOut)
{
	int n = 0;
	// 内置植物：与 game.cpp 载入循环保持一致（i < towerCOUNT - 1）
	for (int i = 0; i < towerCOUNT - 1; ++i)
	{
		wchar_t path[MAX_PATH];
		swprintf_s(path, _countof(path), L"./img/Cards/card_%d.png", i + 1);
		if (!fileExist(path)) continue;
		if (n >= maxOut) break;
		out[n].type = i + 1;
		if (i == WAN_DOU)        wcscpy_s(out[n].name, L"射手");
		else if (i == XIANG_RI_KUI) wcscpy_s(out[n].name, L"收集者");
		else swprintf_s(out[n].name, _countof(out[n].name), L"生物%d", i + 1);
		out[n].cost = cardCostForType(i + 1);
		wcscpy_s(out[n].imgPath, path);
		n++;
	}
	// 导入角色（USER_ENTITY）
	const wchar_t* imp = GetImportedEntityPath();
	if (imp && imp[0] && n < maxOut)
	{
		out[n].type = USER_ENTITY + 1;
		wcscpy_s(out[n].name, L"导入角色");
		out[n].cost = cardCostForType(USER_ENTITY + 1);
		wcscpy_s(out[n].imgPath, imp);
		n++;
	}
	// 英雄（最多 13 个）：每个配好图的英雄各一张卡
	for (int h = 0; h < MAX_HEROES; ++h)
	{
		std::wstring cardPath, framePath, bulletPath;
		const bool hasCard   = HeroCardPathOnDisk(h, cardPath);
		const bool hasFrames = HeroFramePath(h, 0, 1, framePath) || HeroFramePath(h, 1, 1, framePath);
		if (!hasCard && !hasFrames) continue;   // 这个英雄还没配图，不出现在卡池里
		const bool ranged = HeroBulletPathOnDisk(h, bulletPath);   // 配了子弹图 = 远程英雄
		if (n >= maxOut) break;
		out[n].type = HERO_TYPE + h + 1;
		swprintf_s(out[n].name, _countof(out[n].name), L"英雄%d（%ls）",
				   h + 1, ranged ? L"远程" : L"近战");
		out[n].cost = cardCostForType(out[n].type);
		wcscpy_s(out[n].imgPath, (hasCard ? cardPath : framePath).c_str());
		n++;
	}
	return n;
}

// 卡牌 type → 卡面图（拖动跟手预览用）
IMAGE* imageForCardType(int type)
{
	for (int i = 0; i < cardCount; i++)
	{
		if (cards[i].type == type) return cards[i].image;
	}
	return NULL;
}

// 卡牌 type →能量消耗（放置扣费和卡面标价都走这里，保证两处永远一致）
int costForCardType(int type)
{
	// 直接读数值表，而不是读 cards[] 里 buildCardList 时的快照 ——
	// 这样不管谁先改了数值，卡面标价和实际扣费都永远一致。
	int idx = 0;
	if (heroTypeIndex(type, idx)) return g_heroStat[idx].cost;   // 英雄各自定价

	for (int i = 0; i < cardCount; i++)
	{
		if (cards[i].type == type) return cards[i].cost;
	}
	return DEFAULT_CARD_COST;
}

// 已放置生物当前该画的图：自定义生物是静态图，内置生物才走序列帧
IMAGE* imageForPlacedType(int type, int frameIndex)
{
	if (type == USER_ENTITY + 1)
	{
		return userEntityLoaded ? &imgUserEntity : NULL;
	}
	const int towerIndex = type - 1;
	if (towerIndex < 0 || towerIndex >= towerCOUNT) return NULL;
	if (frameIndex < 0 || frameIndex >= 20) frameIndex = 0;
	IMAGE* image = imgtower[towerIndex][frameIndex];
	if (image == NULL) image = imgtower[towerIndex][0];
	return image;
}

void gameInit()
{
	// 注意：第三个参数 1 = EW_SHOWCONSOLE，会额外弹出一个黑色控制台窗口；
	// 这里去掉该标志，保证运行游戏时只出现一个游戏窗口。
	initgraph(WIN_WIDTH, WIN_HEIGHT);

	//加载游戏背景图片
	//把字符集修改成多字符字符集
	loadimage(&imgBg, L"./img/map.jpg");
	loadimage(&imgBar, L"./img/bar5.png");

	memset(imgtower, 0, sizeof(imgtower));
	//初始化tower卡牌
	memset(map, 0, sizeof(map));
	wchar_t name[64];
	for (int i = 0; i < towerCOUNT - 1; i++)
	{
		//生成tower卡牌的文件名
		swprintf_s(name, _countof(name), L"./img/Cards/card_%d.png", i + 1);//加tower向枚举类型中加
		loadimage(&imgCards[i], name);

		for (int j = 0; j < 20; j++)
		{
			swprintf_s(name, _countof(name), L"./img/tower/%d/%d.png", i, j + 1);
			//先判断这个文件是否存在
			//定义一个接口
			if (fileExist(name))
			{
				imgtower[i][j] = new IMAGE;
				//.....加载
				loadimage(imgtower[i][j], name);
			}
			else
			{
				break;
			}
		}
	}
	const wchar_t* importedPath = GetImportedEntityPath();
	if (importedPath) {
		loadimage(&imgUserEntity, importedPath, 64, 76, true);
		removeWhiteBackground(&imgUserEntity);
		userEntityLoaded = imgUserEntity.getwidth() > 0 && imgUserEntity.getheight() > 0;
		if (userEntityLoaded) imgCards[USER_ENTITY] = imgUserEntity;
	}
	curtower = 0;
	sunshine = SUNSHINE_START;
	g_sunIncomeLastTick = 0;   // 重能量结算基准，避免重开游戏时按离线时长补发
	// 胜利条件：本局累计收集量（不是余额）清零重置
	g_sunshineTotal = 0;
	g_gameResult = GAME_RESULT_NONE;

	memset(balls, 0, sizeof(balls));
	for (int i = 0; i < 29; i++)
	{
		swprintf_s(name, _countof(name), L"./img/sunshine/%d.png", i + 1);
		loadimage(&imgSunshineBall[i], name);
	}

	//配置随机种子，让能量真正的随机
	srand((unsigned)time(NULL));

	//设置字体
	LOGFONT f;
	gettextstyle(&f);
	f.lfHeight = 30;
	f.lfWeight = 15;
	wcscpy_s(f.lfFaceName, L"Segoe UI Black");
	//抗锯齿
	f.lfQuality = ANTIALIASED_QUALITY;
	settextstyle(&f);
	setbkmode(TRANSPARENT);
	setcolor(BLACK);

	//初始化僵尸数据
	memset(zms, 0, sizeof(zms));
	int i = 0;
	for (i = 0; i < 22; i++)
	{
		swprintf_s(name, _countof(name), L"./img/zm/%d.png", i + 1);
		loadimage(&imgZM[i], name);
	}

	loadimage(&imgBulletNormal, L"./img/bullets/bullet_normal.png");
	memset(bullets, 0, sizeof(bullets));

	//初始化tower子弹的帧图片数组
	loadimage(&imgBallBlast[3], L"./img/bullets/bullet_blast.png");
	for (int i = 0; i < 3; i++)
	{
		float k = (i + 1) * 0.2;
		loadimage(&imgBallBlast[i], L"./img/bullets/bullet_blast.png",
			imgBallBlast[3].getwidth() * k,
			imgBallBlast[3].getheight() * k, true);
	}

	for (int i = 0; i < 20; i++)
	{
		swprintf_s(name, _countof(name), L"./img/zm.dead/%d.png", i + 1);
		loadimage(&imgZMDead[i], name);
	}

	for (int i = 0; i < 21; i++)
	{
		swprintf_s(name, _countof(name), L"./img/zm_eat/%d.png", i + 1);
		loadimage(&imgZMEat[i], name);
	}

	//载入启动器「设计角色」配置的自定义生物与子弹，并组装工具栏卡槽
	loadCustomBiologys();
	loadHero();
	buildCardList();

	// 每局开局重置 13 个英雄实体（全局变量不会在重开游戏时自动清零）
	for (int h = 0; h < MAX_HEROES; h++)
	{
		g_heroUnits[h] = Hero{};
		g_heroUnits[h].kind = h;
		g_heroUnits[h].cost = g_heroStat[h].cost;
		g_heroUnits[h].dmg  = g_heroStat[h].dmg;
	}
	g_heroHeldIndex = -1;
}


void drawZM()
{
	int zmCount = sizeof(zms) / sizeof(zms[0]);
	for (int i = 0; i < zmCount; i++)
	{
		if (zms[i].used)
		{
			//自定义敌人：按状态播序列帧（行走 6 帧 / 攻击 8 帧）
			if (zms[i].customType > 0 && zms[i].customType <= CUSTOM_MAX)
			{
				const EnemyArt& art = g_enemyArt[zms[i].customType - 1];
				IMAGE* custom = NULL;
				int drawX = zms[i].x;

				if (zms[i].dead)
				{
					//没有专门的死亡序列帧：沿用最后一次的姿态，靠下沉 + 帧计数淡出表现"倒地"
					custom = (art.attackCount > 0) ? art.attack[0]
					                                : (art.walkCount > 0 ? art.walk[0] : NULL);
				}
				else if (zms[i].eating)
				{
					//啃食：优先播攻击动画（8 帧），没传攻击动画就退回行走动画
					if (art.attackCount > 0)
						custom = art.attack[zms[i].frameIndex % art.attackCount];
					else if (art.walkCount > 0)
						custom = art.walk[zms[i].frameIndex % art.walkCount];
				}
				else
				{
					//行走：优先播行走动画（6 帧），只传了攻击动画时也能跑
					if (art.walkCount > 0)
						custom = art.walk[zms[i].frameIndex % art.walkCount];
					else if (art.attackCount > 0)
						custom = art.attack[zms[i].frameIndex % art.attackCount];
				}

				if (custom != NULL)
				{
					int drawY = zms[i].y - custom->getheight();
					if (zms[i].dead)
					{
						//死亡：往下沉一点，配合 updataZM 的帧计数（下沉即"倒地"）
						drawY += zms[i].frameIndex * 2;
					}
					else if (zms[i].eating)
					{
						drawX += (zms[i].frameIndex % 2) ? 2 : -2;//啃食时轻微抖动
					}
					putimagePNG(drawX, drawY, custom);
				}
				continue;
			}

			IMAGE* img = NULL;
			if (zms[i].dead)  img = imgZMDead;
			else if (zms[i].eating) img = imgZMEat;
			else img = imgZM;
			img += zms[i].frameIndex;


			putimagePNG(
				zms[i].x,
				zms[i].y - img->getheight(),
				img);
		}
	}
}
// 圆角"玻璃"面板：深色主体 + 绿色描边 + 柔和投影，用于顶部 HUD（可选居中文字）
static void DrawGlassPanel(int left, int top, int right, int bottom,
                           const wchar_t* label, COLORREF textColor)
{
	const int r = 12;                       // 圆角半径
	setfillcolor(RGB(10, 14, 18));          // 投影（略微下移）
	solidroundrect(left + 2, top + 3, right + 2, bottom + 3, r, r);
	setfillcolor(RGB(28, 38, 48));          // 主体
	fillroundrect(left, top, right, bottom, r, r);
	setlinecolor(RGB(128, 217, 138));       // 描边
	setlinestyle(PS_SOLID, 2);
	roundrect(left, top, right, bottom, r, r);
	setlinestyle(PS_SOLID, 1);
	if (label)
	{
		settextcolor(textColor);
		settextstyle(18, 0, L"Microsoft YaHei UI");
		const int tw = textwidth(label);
		const int th = textheight(label);
		outtextxy((left + right) / 2 - tw / 2, (top + bottom) / 2 - th / 2, label);
	}
}

// 画一个半透明矩形（EasyX 的 solidrectangle 不透明度不可调，这里直接混合显存）。
// 用于能量不够 → 卡牌压暗」这类需要透出底图的遮罩。
// 注意 COLORREF 是 0x00bbggrr，而显存像素是 0xAArrggbb，分量顺序要倒过来。
void fillRectAlpha(int left, int top, int right, int bottom, COLORREF color, int alpha)
{
	if (alpha <= 0) return;
	if (alpha > 255) alpha = 255;

	const int gw = getwidth(), gh = getheight();
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > gw) right = gw;
	if (bottom > gh) bottom = gh;
	if (left >= right || top >= bottom) return;

	DWORD* dst = GetImageBuffer();
	if (dst == NULL) return;

	// COLORREF 是 0x00bbggrr（低位是红），必须用这三个宏拆，直接移位会把 R/B 弄反
	const int cr = GetRValue(color);
	const int cg = GetGValue(color);
	const int cb = GetBValue(color);

	for (int iy = top; iy < bottom; iy++)
	{
		DWORD* row = dst + iy * gw;
		for (int ix = left; ix < right; ix++)
		{
			DWORD& p = row[ix];
			const int br = (p >> 16) & 0xff;
			const int bg = (p >> 8) & 0xff;
			const int bb = p & 0xff;
			p = ((cr * alpha + br * (255 - alpha)) / 255) << 16
				| ((cg * alpha + bg * (255 - alpha)) / 255) << 8
				| ((cb * alpha + bb * (255 - alpha)) / 255);
		}
	}
}

// ---- 屏幕提示条（例如能量不足"）：显示一段时间后自动消失 ----
wchar_t g_toastText[64] = L"";
int     g_toastTicks = 0;      // 剩余显示帧数（updateWindow 每帧减 1）

void showToast(const wchar_t* text)
{
	wcscpy_s(g_toastText, _countof(g_toastText), text);
	g_toastTicks = 90;         // 约 1.8 秒（主循环约 20ms 刷新一帧）
}

void updateWindow()
{
	BeginBatchDraw();//开始缓冲
	putimage(0, 0, &imgBg);

	// ---- 顶部 HUD：退出游戏按钮 + 胜利进度（圆角玻璃面板）----
	{
		LOGFONT hf; gettextstyle(&hf);

		// 退出游戏按钮（点了 = 中途离场，本局判定为失败）
		DrawGlassPanel(16, 16, 140, 54, L"退出游戏", WHITE);

		// 胜利进度面板：本局累计收集的能量 / 10000（达标即胜利）
		// 右侧收到 248，避免被工具栏图片(imgBar)遮挡数字
		int shown = g_sunshineTotal;
		if (shown < 0) shown = 0;
		if (shown > SUNSHINE_WIN_TOTAL) shown = SUNSHINE_WIN_TOTAL;
		wchar_t timeText[32];
		swprintf_s(timeText, _countof(timeText), L"%d/10000", shown);
		DrawGlassPanel(150, 16, 248, 54, nullptr, RGB(134, 217, 138));
		settextcolor(RGB(134, 217, 138));
		settextstyle(16, 0, L"Microsoft YaHei UI");
		const int tw = textwidth(timeText);
		const int th = textheight(timeText);
		outtextxy((150 + 248) / 2 - tw / 2, (16 + 54) / 2 - th / 2, timeText);

		// 能量收入面板：每 2 秒自动到账（基础 10 + 每个产能量生物 10）
		wchar_t incomeText[32];
		swprintf_s(incomeText, _countof(incomeText), L"能量 +%d/2s", sunIncomePer2s());
		DrawGlassPanel(16, 62, 248, 100, incomeText, RGB(255, 236, 150));

		settextstyle(&hf);   // 还原字体，避免影响后续能量数值绘制
	}

	putimagePNG(250, 0, &imgBar);

	// ---- 工具栏卡牌：可翻页卡槽（内置卡牌 + 自定义防御/攻击生物）----
	{
		if (cardPage >= cardPageCount()) cardPage = cardPageCount() - 1;
		if (cardPage < 0) cardPage = 0;

		for (int i = 0; i < CARD_PER_PAGE; i++)
		{
			const int slotIndex = cardPage * CARD_PER_PAGE + i;
			if (slotIndex >= cardCount) break;

			const int x = 338 + i * 65;
			const int y = 6;
			IMAGE* cardImage = cards[slotIndex].image;
			if (cardImage == NULL) continue;

			// 经济系统：这张卡现在买不买得起
			const int cost = cards[slotIndex].cost;
			const bool affordable = (sunshine >= cost);
			int cardW = 60, cardH = 84;   // 自定义卡是自绘尺寸，内置卡下面按实际图改

			int heroIdx = -1;
			if (heroTypeIndex(cards[slotIndex].type, heroIdx))
			{
				//英雄卡没有现成卡牌图：自绘一张圆角小卡片（投影 + 描边）+ 英雄卡面
				setfillcolor(RGB(10, 14, 18));
				solidroundrect(x + 2, y + 3, x + 62, y + 87, 10, 10);
				setfillcolor(RGB(30, 34, 40));
				fillroundrect(x, y, x + 60, y + 84, 10, 10);
				setlinecolor(RGB(70, 78, 90));
				setlinestyle(PS_SOLID, 1);
				roundrect(x, y, x + 60, y + 84, 10, 10);
				const int imageW = cardImage->getwidth();
				const int imageH = cardImage->getheight();
				putimagePNG(x + (60 - imageW) / 2, y + (84 - imageH) / 2, cardImage);
			}
			else
			{
				putimage(x, y, cardImage);
				cardW = cardImage->getwidth();
				cardH = cardImage->getheight();
			}

			// 能量不够 → 整张卡压暗，一眼看出这张卡现在放不起
			if (!affordable)
			{
				fillRectAlpha(x, y, x + cardW, y + cardH, RGB(14, 16, 22), 155);
			}

			// 卡面底部标出这张卡的能量消耗（买得起金色，买不起红色）
			{
				LOGFONT cardFont; gettextstyle(&cardFont);
				const COLORREF cardColor = gettextcolor();

				const int barH = 18;
				const int barTop = y + cardH - barH;
				setfillcolor(RGB(16, 20, 26));
				solidrectangle(x, barTop, x + cardW - 1, y + cardH - 1);
				setlinecolor(affordable ? RGB(122, 96, 42) : RGB(104, 48, 48));
				rectangle(x, barTop, x + cardW - 1, y + cardH - 1);

				wchar_t costText[16];
				swprintf_s(costText, _countof(costText), L"%d", cost);
				setbkmode(TRANSPARENT);
				settextstyle(13, 0, L"Microsoft YaHei UI");
				settextcolor(affordable ? RGB(255, 236, 150) : RGB(226, 106, 106));
				const int cw = textwidth(costText);
				outtextxy(x + (cardW - cw) / 2, barTop + 2, costText);

				settextstyle(&cardFont);
				settextcolor(cardColor);
			}

			if (curtower == cards[slotIndex].type)//选中高亮：圆角发光描边
			{
				setlinecolor(RGB(150, 255, 180));
				setlinestyle(PS_SOLID, 3);
				roundrect(x - 3, y - 3, x + 63, y + 87, 12, 12);
				setlinecolor(RGB(128, 217, 138));
				setlinestyle(PS_SOLID, 1);
				roundrect(x - 1, y - 1, x + 61, y + 85, 10, 10);
			}
		}

		// 卡槽超过一页时显示翻页箭头与页码
		if (cardPageCount() > 1)
		{
			LOGFONT savedFont;
			gettextstyle(&savedFont);
			const COLORREF savedColor = gettextcolor();

			setfillcolor(RGB(28, 38, 48));
			fillroundrect(856, 26, 886, 60, 8, 8);
			fillroundrect(896, 26, 926, 60, 8, 8);
			setlinecolor(RGB(134, 217, 138));
			setlinestyle(PS_SOLID, 1);
			roundrect(856, 26, 886, 60, 8, 8);
			roundrect(896, 26, 926, 60, 8, 8);

			settextstyle(18, 0, L"Microsoft YaHei UI");
			settextcolor(cardPage > 0 ? WHITE : RGB(110, 116, 126));
			outtextxy(863, 30, L"‹");
			settextcolor(cardPage + 1 < cardPageCount() ? WHITE : RGB(110, 116, 126));
			outtextxy(903, 30, L"›");

			wchar_t pageText[16];
			swprintf_s(pageText, _countof(pageText), L"%d/%d", cardPage + 1, cardPageCount());
			settextstyle(16, 0, L"Microsoft YaHei UI");
			settextcolor(RGB(150, 156, 166));
			outtextxy(878, 68, pageText);

			settextstyle(&savedFont);
			settextcolor(savedColor);
		}
	}

	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 9; j++)
		{
			if (map[i][j].type > 0)
			{
				//内置生物走序列帧，自定义生物与导入角色走静态图（统一在这里分发，避免越界）
				IMAGE* image = imageForPlacedType(map[i][j].type, map[i][j].frameIndex);
				if (image != NULL) putimagePNG(map[i][j].x, map[i][j].y, image);
			}
		}
	}
	//渲染拖动过程中的生物
	if (curtower)
	{
		IMAGE* img = imageForCardType(curtower);
		if (img != NULL) putimagePNG(curX - img->getwidth() / 2, curY - img->getheight() / 2, img);
	}

	// 渲染英雄：逐个英雄画自己的休闲 / 攻击两套 8 帧动画（拖动移动时跟随光标）
	for (int h = 0; h < MAX_HEROES; h++)
	{
		Hero& hero = g_heroUnits[h];
		if (!hero.placed && !hero.held) continue;
		HeroArt& art = g_heroArt[h];

		IMAGE* heroImg = nullptr;
		if (hero.state == 1 && art.attackCount > 0)
			heroImg = art.attack[hero.frameIndex % art.attackCount];
		else if (art.idleCount > 0)
			heroImg = art.idle[hero.frameIndex % art.idleCount];
		else if (art.attackCount > 0)
			heroImg = art.attack[hero.frameIndex % art.attackCount];
		if (heroImg != nullptr) putimagePNG(hero.x, hero.y, heroImg);

		// 英雄血条（顶部一小条，方便看清还能扛多久）
		const int maxHp = g_heroStat[h].hp;
		if (hero.placed && hero.hp > 0 && maxHp > 0)
		{
			const int bw = HERO_W, bx = hero.x, by = hero.y - 8;
			const int ratio = (hero.hp > maxHp) ? maxHp : hero.hp;
			const int fw = bw * ratio / maxHp;
			setfillcolor(RGB(40, 40, 48));
			solidrectangle(bx, by, bx + bw - 1, by + 4);
			setfillcolor(RGB(120, 220, 255));
			solidrectangle(bx, by, bx + fw - 1, by + 4);
		}
	}


	wchar_t scoreText[8];
	swprintf_s(scoreText, _countof(scoreText), L"%d", sunshine);
	settextstyle(20, 0, L"Microsoft YaHei UI");
	settextcolor(RGB(255, 236, 150));//能量金，配合工具栏更醒目
	outtextxy(276, 67, scoreText);//输出能量分数

	drawZM();//渲染僵尸


	//渲染能量
	int bulletMax = sizeof(bullets) / sizeof(bullets[0]);
	for (int i = 0; i < bulletMax; i++)
	{
		if (bullets[i].used)
		{
			if (bullets[i].blast)
			{
				IMAGE* img = &imgBallBlast[bullets[i].frameIndex];
				putimagePNG(bullets[i].x, bullets[i].y, img);
			}
			else
			{
				// 远程英雄的子弹用它自己配的图；其余（豌豆等）用游戏默认子弹
				IMAGE* bulletImage = &imgBulletNormal;
				int hIdx = 0;
				if (heroTypeIndex(bullets[i].bulletKind, hIdx) &&
					g_heroArt[hIdx].bulletLoaded && g_heroArt[hIdx].bullet != NULL)
				{
					bulletImage = g_heroArt[hIdx].bullet;
				}
				putimagePNG(bullets[i].x, bullets[i].y, bulletImage);
			}
		}
	}



	// ---- 屏幕提示条（"能量不足：需要 100，当前 60" 之类）----
	if (g_toastTicks > 0)
	{
		g_toastTicks--;
		LOGFONT savedFont; gettextstyle(&savedFont);
		const COLORREF savedColor = gettextcolor();

		setbkmode(TRANSPARENT);
		settextstyle(18, 0, L"Microsoft YaHei UI");
		const int tw = textwidth(g_toastText);
		const int th = textheight(g_toastText);
		const int padX = 20, padY = 11;
		// 落在草坪下方的空区，不挡战斗画面
		const int left = (WIN_WIDTH - (tw + padX * 2)) / 2;
		const int top = 508;
		const int right = left + tw + padX * 2;
		const int bottom = top + th + padY * 2;

		setfillcolor(RGB(20, 24, 30));
		solidroundrect(left + 2, top + 3, right + 2, bottom + 3, 10, 10);
		fillroundrect(left, top, right, bottom, 10, 10);
		setlinecolor(RGB(226, 106, 106));
		setlinestyle(PS_SOLID, 2);
		roundrect(left, top, right, bottom, 10, 10);
		setlinestyle(PS_SOLID, 1);
		settextcolor(RGB(240, 198, 198));
		outtextxy(left + padX, top + padY, g_toastText);

		settextstyle(&savedFont);
		settextcolor(savedColor);
	}

	EndBatchDraw();//结束双缓冲
}

// 前向声明：把第 h 个英雄放到某个草坪格（定义在 updateHero 附近）
static void placeHeroAt(int row, int col, int h);

void userClick()
{
	ExMessage msg;//参数是消息类型
	static int status = 0;
	//如果消息有值则保存在peekmessage函数中,即为真
	if (peekmessage(&msg))
	{
		if (msg.message == WM_CLOSE || msg.message == WM_DESTROY)
		{
			// 直接关窗口也算中途离场 → 判负
			g_gameRunning = false;
			g_gameResult = GAME_RESULT_LOSE_EXIT;
			return;
		}
		if (msg.message == WM_LBUTTONDOWN)//WM_LBUTTONDOWN左键按下去的意思
		{
		// 游戏体内的「退出游戏」按钮：中途退出 → 本局判定为失败，随后回到主菜单
		if (msg.x >= 16 && msg.x <= 140 && msg.y >= 16 && msg.y <= 54)
		{
			g_gameRunning = false;
			g_gameResult = GAME_RESULT_LOSE_EXIT;
			return;
		}
			//工具栏翻页箭头（要先于卡牌命中判断，两者横向范围有重叠）
			if (msg.y >= 26 && msg.y <= 60 && msg.x >= 856 && msg.x <= 886)
			{
				if (cardPage > 0) cardPage--;
				return;
			}
		if (msg.y >= 26 && msg.y <= 60 && msg.x >= 896 && msg.x <= 926)
		{
			if (cardPage + 1 < cardPageCount()) cardPage++;
			return;
		}
			// 移动英雄：点到已放置的英雄身上 → 拿起（无需先选卡）
			for (int h = MAX_HEROES - 1; h >= 0; h--)   // 从后往前，后放的在上方优先被点中
			{
				Hero& hero = g_heroUnits[h];
				if (hero.placed && !hero.held &&
					msg.x >= hero.x && msg.x <= hero.x + HERO_W &&
					msg.y >= hero.y && msg.y <= hero.y + HERO_H)
				{
					hero.held = true;
					g_heroHeldIndex = h;
					curX = msg.x; curY = msg.y;
					status = 1;   // 复用拖动状态，让 WM_MOUSEMOVE 跟随光标
					return;
				}
			}
		if (msg.x > 338 && msg.x < 338 + 65 * CARD_PER_PAGE && msg.y < 96)//判断点的是第几张卡
			{
				const int slotIndex = cardPage * CARD_PER_PAGE + (msg.x - 338) / 65;
				if (slotIndex >= 0 && slotIndex < cardCount)
				{
					// 经济系统：能量不够就选不了这张卡（工具栏里对应卡面是压暗 + 红字）
					if (sunshine < cards[slotIndex].cost)
					{
						wchar_t tip[64];
						swprintf_s(tip, _countof(tip), L"能量不足：需要 %d，当前 %d",
							cards[slotIndex].cost, sunshine);
						showToast(tip);
						curtower = 0;
						status = 0;
						return;
					}
					status = 1;
					curtower = cards[slotIndex].type;
				}
			}
			// 点击草坪区域不再有"收集能量"逻辑：能量改为每 2 秒自动到账
		}
		else if (msg.message == WM_MOUSEMOVE && status == 1)//WM_MOUSEMOVE鼠标移动
		{
			//记录当前位置
			curX = msg.x;
			curY = msg.y;
			if (g_heroHeldIndex >= 0)   // 移动英雄时让英雄贴着光标
			{
				Hero& hero = g_heroUnits[g_heroHeldIndex];
				hero.x = curX - HERO_W / 2;
				hero.y = curY - HERO_H / 2;
			}
		}
		//鼠标抬动tower就放置下去
		else if (msg.message == WM_LBUTTONUP)
		{
			// 正在移动英雄（拿起状态）→ 落到合法草坪格则移动，否则回原位
			if (g_heroHeldIndex >= 0)
			{
				Hero& hero = g_heroUnits[g_heroHeldIndex];
				if (msg.x > 256 && msg.x < 985 && msg.y > 179 && msg.y < 489)
				{
					int row = (msg.y - 179) / 102;
					int col = (msg.x - 256) / 81;
					if (row >= 0 && row < 3 && col >= 0 && col < 9)
						placeHeroAt(row, col, g_heroHeldIndex);
					else
						placeHeroAt(hero.cellRow, hero.cellCol, g_heroHeldIndex);  // 非法落点：回原格
				}
				else
				{
					placeHeroAt(hero.cellRow, hero.cellCol, g_heroHeldIndex);      // 落在草坪外：回原格
				}
				hero.held = false;
				g_heroHeldIndex = -1;
				status = 0;
				curtower = 0;
				return;
			}

			if (msg.x > 256 && msg.y > 179 && msg.y < 489)
			{
				int row = (msg.y - 179) / 102;
				int col = (msg.x - 256) / 81;
				const bool inGrid = (row >= 0 && row < 3 && col >= 0 && col < 9);

				// 英雄卡：同一个英雄场上只能有一个；该英雄已在场则拦截
				int heroIdx = -1;
				if (heroTypeIndex(curtower, heroIdx))
				{
					Hero& hero = g_heroUnits[heroIdx];
					if (hero.placed)
					{
						wchar_t tip[64];
						swprintf_s(tip, _countof(tip), L"英雄%d 已经在场上了", heroIdx + 1);
						showToast(tip);
					}
					else if (inGrid && map[row][col].type == 0)
					{
						const int cost = costForCardType(curtower);
						if (sunshine < cost)
						{
							showToast(L"能量不足，无法放置英雄");
						}
						else
						{
							sunshine -= cost;           // 经济系统：真的放下了才扣能量
							placeHeroAt(row, col, heroIdx);
							hero.placed  = true;
							hero.kind    = heroIdx;
							hero.hp      = g_heroStat[heroIdx].hp;
							hero.dmg     = g_heroStat[heroIdx].dmg;
							hero.cost    = g_heroStat[heroIdx].cost;
							hero.state      = 0;
							hero.frameIndex = 0;
							hero.frameTimer = 0;
							hero.atkTimer   = 0;
						}
					}
				}
				// 普通植物卡
				else if (inGrid && curtower != 0 && map[row][col].type == 0)
				{
					// 防止在英雄所在格上叠植物
					bool heroOccupied = false;
					for (int h = 0; h < MAX_HEROES; h++)
					{
						if (g_heroUnits[h].placed &&
							g_heroUnits[h].cellRow == row && g_heroUnits[h].cellCol == col)
						{
							heroOccupied = true;
							break;
						}
					}
					if (heroOccupied)
					{
						showToast(L"该格已被英雄占据");
					}
					else
					{
						const int cost = costForCardType(curtower);
						if (sunshine < cost)
						{
							// 拖的过程中能量只会增加不会减少，理论上到不了这里，留个兜底
							showToast(L"能量不足，无法放置");
						}
						else
						{
							sunshine -= cost;           // 经济系统：真的放下了才扣能量
							map[row][col].type = curtower;
							map[row][col].frameIndex = 0;
							map[row][col].timer = 0;        //射击节奏计时
							map[row][col].catched = false;  //复用格子要清掉上一株的啃食状态
							map[row][col].hp = DEFAULT_PLANT_HP;

							map[row][col].x = 256 + col * 81;
							map[row][col].y = 179 + row * 102 + 14;
						}
					}
				}
			}
			curtower = 0;
			status = 0;

		}
	}
}


// 统计场上"产能量"生物的数量（向日葵在 map 里的 type 存的是 XIANG_RI_KUI + 1）
int countSunProducers()
{
	int n = 0;
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 9; j++)
		{
			if (map[i][j].type == XIANG_RI_KUI + 1) n++;
		}
	}
	return n;
}

// 每 2 秒到账的能量总量：基础 10 + 每个产能量生物 10
int sunIncomePer2s()
{
	return SUNSHINE_BASE_INCOME + SUNSHINE_PER_PRODUCER * countSunProducers();
}

// 自动能量收入：按真实时间每 2 秒结算一次（帧率快慢不影响收益速度）
// 已删除"天上掉落、能量 + 点击收集"的旧玩法，向日葵也不再掉落能量球，
// 改为：每放置一个向日葵，每 2 秒的自动收入就 +10。
void autoSunshineIncome()
{
	const DWORD now = GetTickCount();

	if (g_sunIncomeLastTick == 0) { g_sunIncomeLastTick = now; return; }   // 首帧只记录基准时间

	// while 而非 if：卡顿/掉帧后也能把欠下的结算补上（DWORD 回绕也安全）
	while ((DWORD)(now - g_sunIncomeLastTick) >= (DWORD)SUNSHINE_INCOME_INTERVAL_MS)
	{
		g_sunIncomeLastTick += SUNSHINE_INCOME_INTERVAL_MS;
		const int income = sunIncomePer2s();
		sunshine += income;          // 余额：放卡会花掉
		g_sunshineTotal += income;   // 累计收集量：只增不减，用于判定胜利
	}
}


void creatZM()
{
	static int zmFre = 500;
	static int count = 0;
	count++;
	if (count > zmFre)
	{
		count = 0;
		zmFre = rand() % 200 + 300;
	}
	int i = 0;
	int zmMax = sizeof(zms) / sizeof(zms[0]);
	for (i = 0; i < zmMax && zms[i].used; i++);
	if (i < zmMax)
	{
		memset(&zms[i], 0, sizeof(zms[i]));
		zms[i].used = true;
		zms[i].x = WIN_WIDTH;
		zms[i].row = rand() % 3;
		zms[i].y = 172 + (1 + zms[i].row) * 100;
		zms[i].speed = 1;
		zms[i].blood = DEFAULT_ZOMBIE_HP;
		zms[i].dmg = DEFAULT_ZOMBIE_DMG;
		zms[i].dead = false;

		//若「设计角色 → 敌人生物」里配了图，则有一半概率派这种自定义敌人登场；
		//速度、移动、死亡规则与默认敌人一致，但血量和啃食伤害用它自己配的数值。
		//注意：编号可能不连续（用户跳号配置 / 删掉中间某个），所以从已配置列表里挑，
		//不能再用 1 + rand() % customEnemyCount 那种连续编号假设。
		if (g_enemySpawnCount > 0 && (rand() % 2) == 0)
		{
			const int idx = g_enemySpawnList[rand() % g_enemySpawnCount];
			zms[i].customType = idx + 1;
			if (idx < kMaxBioSlots)
			{
				const int hp  = g_bioStats[BIO_ENEMY][idx].hp;
				const int dmg = g_bioStats[BIO_ENEMY][idx].dmg;
				zms[i].blood = (hp  > 0) ? hp  : DEFAULT_ZOMBIE_HP;
				zms[i].dmg   = (dmg > 0) ? dmg : DEFAULT_ZOMBIE_DMG;
			}
		}
	}
}


void updataZM()
{
	int zmMax = sizeof(zms) / sizeof(zms[0]);

	static int count = 0;
	count++;
	if (count > 2)
	{
		count = 0;
		//更新敌人的位置
		for (int i = 0; i < zmMax; i++)
		{

			if (zms[i].used)
			{
				zms[i].x -= zms[i].speed;
				if (zms[i].x < 170)
				{
					// 敌人突破防线 → 判负（提示框统一在主循环结束后弹，避免和窗口销毁冲突）
					g_gameRunning = false;
					g_gameResult = GAME_RESULT_LOSE_BREACH;
					return;
				}
			}
		}
	}


	static int count2 = 0;
	count2++;
	if (count2 > 4)
	{
		count2 = 0;
		for (int i = 0; i < zmMax; i++)
		{
			if (zms[i].used)
			{
				if (zms[i].dead)
				{
					zms[i].frameIndex++;
					if (zms[i].frameIndex >= 20)
					{
						zms[i].used = false;
					}
				}
				else if (zms[i].eating)
				{
					zms[i].frameIndex = (zms[i].frameIndex + 1) % 21;
				}
				else
				{
					zms[i].frameIndex = (zms[i].frameIndex + 1) % 22;
				}
			}
		}
	}
}


void shoot()
{
	int lines[3] = { 0 };
	int zmCount = sizeof(zms) / sizeof(zms[0]);
	int bulletMax = sizeof(bullets) / sizeof(bullets[0]);
	int dangerX = WIN_WIDTH - imgZM[0].getwidth();
	for (int i = 0; i < zmCount; i++)
	{
		if (zms[i].used && zms[i].x < dangerX)
		{
			lines[zms[i].row] = 1;
		}
	}
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 9; j++)
		{
			const int type = map[i][j].type;
			if (type == WAN_DOU + 1 && lines[i])
			{
				static int count = 0;
				count++;
				if (count > 20)
				{
					count = 0;
					int k = 0;
					for (k = 0; k < bulletMax && bullets[k].used; k++);
					if (k < bulletMax)
					{
						bullets[k].used = true;
						bullets[k].row = i;
						bullets[k].speed = 4;

						bullets[k].blast = false;
						bullets[k].frameIndex = 0;
						bullets[k].bulletKind = 0;          //游戏默认子弹
						bullets[k].damage = DEFAULT_BULLET_DMG;

						int zwX = 256 + j * 81;
						int zwY = 179 + i * 102 + 14;
						bullets[k].x = zwX + imgtower[map[i][j].type - 1][0]->getwidth() - 10;
						bullets[k].y = zwY + 5;
					}
				}
			}
			// （自定义攻击生物已从「设计角色」移除，只有内置豌豆会射击）
		}
	}
}

void updataBullets()
{
	int countMax = sizeof(bullets) / sizeof(bullets[0]);
	for (int i = 0; i < countMax; i++)
	{
		if (bullets[i].used)
		{
			bullets[i].x += bullets[i].speed;
			if (bullets[i].x > WIN_WIDTH)
			{
				bullets[i].used = false;
			}

			//子弹的碰撞爆炸
			if (bullets[i].blast)
			{
				bullets[i].frameIndex++;
				if (bullets[i].frameIndex >= 4)
				{
					bullets[i].used = false;
				}
			}
		}
	}
}

void checkBullet2Zm()
{
	int bCount = sizeof(bullets) / sizeof(bullets[0]);
	int zCount = sizeof(zms) / sizeof(zms[0]);
	for (int i = 0; i < bCount; i++)
	{
		if (bullets[i].used == false || bullets[i].blast)
		{
			continue;
		}
		for (int k = 0; k < zCount; k++)
		{
			//if (zms[i].used == false) continue;
			if (zms[k].used == false)  continue;
			int x1 = zms[k].x + 80;
			int x2 = zms[k].x + 110;
			int x = bullets[i].x;
			if (zms[k].dead == false && bullets[i].row == zms[k].row && x > x1 && x < x2)
			{
				zms[k].blood -= bullets[i].damage;   //伤害由发射方决定（豌豆固定，自定义生物用配的值）
				bullets[i].blast = true;
				bullets[i].speed = 0;

				if (zms[k].blood <= 0)
				{
					zms[k].dead = true;
					zms[k].speed = 0;
					zms[k].frameIndex = 0;
				}


				break;
			}
		}
	}
}

void checkZm2tower()
{

	int zCount = sizeof(zms) / sizeof(zms[0]);
	for (int i = 0; i < zCount; i++)
	{
		if (zms[i].dead) continue;
		int row = zms[i].row;
		for (int k = 0; k < 9; k++)
		{
			if (map[row][k].type == 0)
			{
				continue;
			}
			int towerX = 256 + k * 81;
			int x1 = towerX + 10;
			int x2 = towerX + 60;
			int x3 = zms[i].x + 80;
			if (x3 > x1 && x3 < x2)
			{
				if (map[row][k].catched)
				{
					// 啃食：每次结算扣掉这个敌人的啃食伤害，血量见底就消失
					map[row][k].hp -= (zms[i].dmg > 0 ? zms[i].dmg : 1);
					if (map[row][k].hp <= 0)
					{
						map[row][k].type = 0;
						map[row][k].hp = 0;
						zms[i].eating = false;
						zms[i].frameIndex = 0;
						zms[i].speed = 1;
					}
				}
				else
				{
					map[row][k].catched = true;
					zms[i].eating = true;
					zms[i].speed = 0;
					zms[i].frameIndex = 0;

				}
			}
		}

		// 英雄作为近战单位：僵尸走到英雄身上会啃英雄（英雄掉血），但英雄不阻挡僵尸前进
		for (int h = 0; h < MAX_HEROES; h++)
		{
			Hero& hero = g_heroUnits[h];
			if (!hero.placed || hero.held) continue;
			if (zms[i].row != hero.cellRow) continue;
			const int hx = hero.x;
			const int x1 = hx + 10;
			const int x2 = hx + HERO_W - 12;
			const int x3 = zms[i].x + 80;
			if (x3 > x1 && x3 < x2)
			{
				hero.hp -= (zms[i].dmg > 0 ? zms[i].dmg : 1);
			}
		}
	}
}

void collisionCheck()
{
	checkBullet2Zm();
	checkZm2tower();
}

// 把第 h 个英雄放在某个草坪格（3 行 9 列），并算出它的像素左上角
static void placeHeroAt(int row, int col, int h)
{
	if (h < 0 || h >= MAX_HEROES) return;
	Hero& hero = g_heroUnits[h];
	hero.cellRow = row;
	hero.cellCol = col;
	hero.x = 256 + col * 81;
	hero.y = 179 + row * 102 + 14;
}

// 远程英雄发射一发子弹（用这个英雄自己配的子弹图和伤害）
static void heroShoot(int h, const Hero& hero)
{
	const int bulletMax = (int)(sizeof(bullets) / sizeof(bullets[0]));
	int k = 0;
	for (k = 0; k < bulletMax && bullets[k].used; k++);
	if (k >= bulletMax) return;                  // 子弹池满了，这一发跳过

	bullets[k].used = true;
	bullets[k].row  = hero.cellRow;
	bullets[k].speed = HERO_BULLET_SPEED;
	bullets[k].blast = false;
	bullets[k].frameIndex = 0;
	bullets[k].damage = hero.dmg;                // 伤害走该英雄配的数值
	bullets[k].bulletKind = HERO_TYPE + h + 1;   // 绘制时按这个取英雄自己的子弹图
	bullets[k].x = hero.x + HERO_W - 10;
	bullets[k].y = hero.y + 18;
}

// 英雄实体更新：逐个英雄推进休闲 / 攻击两套 8 帧动画 + 伤害结算
//   配了子弹图 → 远程：按节奏发射自己的子弹；没配 → 近战：贴身砍
void updateHero()
{
	const int zmMax = (int)(sizeof(zms) / sizeof(zms[0]));

	for (int h = 0; h < MAX_HEROES; h++)
	{
		Hero& hero = g_heroUnits[h];
		if (!hero.placed) continue;

		const bool ranged = heroIsRanged(h);
		const bool canFight = !hero.held;         // 被玩家拿在手上移动时不出手
		const int heroCenterX = hero.x + HERO_W / 2;
		const int heroRow = hero.cellRow;

		// 找本行、在射程内、且还活着的僵尸（取最靠近英雄的那个）
		int target = -1;
		int bestDist = 0x7fffffff;
		if (canFight)
		{
			const int range = ranged ? HERO_RANGE_RANGE : HERO_MELEE_RANGE;
			for (int i = 0; i < zmMax; i++)
			{
				if (!zms[i].used || zms[i].dead) continue;
				if (zms[i].row != heroRow) continue;
				const int zx = zms[i].x + 40;          // 僵尸身体大致中心（x 是左边缘）
				// 远程只打自己前方（右侧）的敌人，近战贴身范围内前后都打
				if (ranged && zx < heroCenterX - 30) continue;
				const int dist = abs(zx - heroCenterX);
				if (dist <= range && dist < bestDist)
				{
					bestDist = dist;
					target = i;
				}
			}
		}

		if (target >= 0)
		{
			hero.state = 1;                        // 攻击状态
			hero.frameTimer++;
			if (hero.frameTimer >= 4)              // 约每 80ms 进一帧
			{
				hero.frameTimer = 0;
				hero.frameIndex = (hero.frameIndex + 1) % HERO_FRAMES;
			}
			hero.atkTimer++;
			const int interval = ranged ? HERO_SHOOT_INTERVAL : HERO_ATK_INTERVAL;
			if (hero.atkTimer >= interval)         // 远程：每节奏一发；近战：每节奏一刀
			{
				hero.atkTimer = 0;
				if (ranged)
				{
					heroShoot(h, hero);
				}
				else
				{
					zms[target].blood -= hero.dmg;
					if (zms[target].blood <= 0)
					{
						zms[target].dead = true;
						zms[target].speed = 0;
						zms[target].frameIndex = 0;
					}
				}
			}
		}
		else
		{
			hero.state = 0;                        // 休闲状态
			hero.atkTimer = 0;
			hero.frameTimer++;
			if (hero.frameTimer >= 4)
			{
				hero.frameTimer = 0;
				hero.frameIndex = (hero.frameIndex + 1) % HERO_FRAMES;
			}
		}

		// 英雄阵亡 → 退场
		if (hero.hp <= 0)
		{
			hero.placed = false;
			hero.held = false;
			hero.state = 0;
			hero.frameIndex = 0;
			hero.hp = 0;
			if (g_heroHeldIndex == h) g_heroHeldIndex = -1;

			wchar_t tip[64];
			swprintf_s(tip, _countof(tip), L"英雄%d 倒下了！", h + 1);
			showToast(tip);
		}
	}
}
void updateGame()
{

	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 9; j++)
		{
			//只有内置生物才有序列帧；自定义生物是静态图，不能拿它的 type 去查 imgtower（会越界）
			if (map[i][j].type > 0 && map[i][j].type <= towerCOUNT - 1)
			{
				map[i][j].frameIndex++;
				int towertype = map[i][j].type - 1;
				int index = map[i][j].frameIndex;
				if (index >= 20 || imgtower[towertype][index] == NULL)
				{
					map[i][j].frameIndex = 0;
					continue;
				}
			}
		}
	}
	autoSunshineIncome();//自动能量收入：每 2 秒到账（基础 10 + 每个产能量生物 10）

	creatZM();//创建敌人
	updataZM();//更新敌人的状态

	shoot();//发射tower子弹
	updataBullets();//更新tower子弹

	collisionCheck();//实现tower子弹和僵尸的碰撞
	updateHero();   // 英雄实体：休闲/攻击动画 + 近战伤害结算
}

// ---------------------------------------------------------------------
// 让游戏窗口和启动器「一样大」
// ---------------------------------------------------------------------
// 启动器是按系统 DPI 缩放创建的（例如 200% 缩放下客户区是 1920x1280），
// 而 EasyX 的 initgraph(960,640) 建出来的是 960x640 物理像素，所以在高 DPI
// 屏幕上游戏窗口只有菜单窗口的一半大。
// 这里在进入游戏前把当前线程临时设为「DPI 不感知」：Windows 会自动把 EasyX
// 窗口按系统缩放放大到与启动器一致，并且把鼠标坐标换算回 960x640 逻辑坐标，
// 因此游戏内的坐标、点击判定、图片尺寸全都不用改。退出游戏时恢复原状。
// ---------------------------------------------------------------------
#ifndef DPI_AWARENESS_CONTEXT_UNAWARE
#define DPI_AWARENESS_CONTEXT_UNAWARE ((DPI_AWARENESS_CONTEXT)-1)
#endif
typedef DPI_AWARENESS_CONTEXT(WINAPI* PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
static PFN_SetThreadDpiAwarenessContext GetSetThreadDpiAwarenessContext()
{
	// 动态取函数地址：老系统没有这个 API 时返回空，游戏按原样运行
	static PFN_SetThreadDpiAwarenessContext fn = []() -> PFN_SetThreadDpiAwarenessContext
	{
		HMODULE u32 = GetModuleHandleW(L"user32.dll");
		if (!u32) return nullptr;
		return reinterpret_cast<PFN_SetThreadDpiAwarenessContext>(
			GetProcAddress(u32, "SetThreadDpiAwarenessContext"));
	}();
	return fn;
}

void GameMain()
{
	// 进入游戏：线程切到「DPI 不感知」，让 EasyX 窗口被系统放大到与启动器同样大小
	PFN_SetThreadDpiAwarenessContext setThreadDpi = GetSetThreadDpiAwarenessContext();
	DPI_AWARENESS_CONTEXT prevDpiCtx = nullptr;
	if (setThreadDpi) prevDpiCtx = setThreadDpi(DPI_AWARENESS_CONTEXT_UNAWARE);

	gameInit();//直接进入游戏主体，不再经过菜单界面
	int timer = 0;
	bool flag = true;
	g_gameRunning = true;

	while (g_gameRunning)
	{
		// ---- 胜利判定：本局累计收集的能量达到 10000 ----
		if (g_sunshineTotal >= SUNSHINE_WIN_TOTAL)
		{
			g_gameRunning = false;
			g_gameResult = GAME_RESULT_WIN;
			break;
		}

		userClick();
		timer += getDelay();
		if (timer > 20)
		{
			flag = true;
			timer = 0;
		}
		if (flag)
		{
			flag = false;
			updateWindow();
			updateGame();
		}
	}

	// ---- 结算弹窗：先恢复线程 DPI 上下文，免得对话框被系统拉伸变糊 ----
	// 用宽字符版本，否则在 Unicode 工程里中文提示会变成乱码
	if (setThreadDpi && prevDpiCtx) { setThreadDpi(prevDpiCtx); prevDpiCtx = nullptr; }
	if (g_gameResult == GAME_RESULT_WIN)
	{
		MessageBoxW(NULL, L"恭喜！能量累计收集达到 10000，游戏胜利！", L"游戏胜利", MB_OK);
	}
	else if (g_gameResult == GAME_RESULT_LOSE_EXIT)
	{
		MessageBoxW(NULL, L"你在游戏中途选择了退出，本局判定为失败。", L"游戏失败", MB_OK);
	}
	else if (g_gameResult == GAME_RESULT_LOSE_BREACH)
	{
		MessageBoxW(NULL, L"敌人突破了防线，本局判定为失败。", L"游戏失败", MB_OK);
	}
	g_gameResult = GAME_RESULT_NONE;

	for (int i = 0; i < towerCOUNT; ++i)
	{
		for (int j = 0; j < 20; ++j)
		{
			delete imgtower[i][j];
			imgtower[i][j] = NULL;
		}
	}
	//释放自定义敌人贴图：每名敌人两套动画（行走 6 帧 + 攻击 8 帧；delete NULL 是安全的）
	for (int i = 0; i < CUSTOM_MAX; ++i)
	{
		for (int f = 0; f < ENEMY_WALK_FRAMES; ++f)
		{
			delete g_enemyArt[i].walk[f];     g_enemyArt[i].walk[f] = NULL;
		}
		for (int f = 0; f < ENEMY_ATTACK_FRAMES; ++f)
		{
			delete g_enemyArt[i].attack[f];   g_enemyArt[i].attack[f] = NULL;
		}
		g_enemyArt[i].walkCount   = 0;
		g_enemyArt[i].attackCount = 0;
		g_enemyArt[i].loaded      = false;
	}
	customEnemyCount = 0;
	//释放 13 个英雄的动画帧
	for (int h = 0; h < MAX_HEROES; ++h)
	{
		for (int f = 0; f < HERO_FRAMES; ++f)
		{
			delete g_heroArt[h].attack[f]; g_heroArt[h].attack[f] = NULL;
			delete g_heroArt[h].idle[f];   g_heroArt[h].idle[f]   = NULL;
		}
		delete g_heroArt[h].bullet; g_heroArt[h].bullet = NULL;   // 英雄子弹图（远程英雄）
		g_heroArt[h].attackCount   = 0;
		g_heroArt[h].idleCount     = 0;
		g_heroArt[h].cardLoaded    = false;
		g_heroArt[h].bulletLoaded  = false;
		g_heroArt[h].loaded        = false;
	}
	cardCount = 0;
	

	// 退出游戏：恢复线程的 DPI 上下文，启动器按原来的 DPI 设置继续显示
	if (setThreadDpi && prevDpiCtx) setThreadDpi(prevDpiCtx);

	// 先隐藏 EasyX 窗口再销毁，避免游戏画面闪烁在启动器上
	HWND easyxHwnd = GetHWnd();
	if (easyxHwnd && IsWindow(easyxHwnd)) ShowWindow(easyxHwnd, SW_HIDE);
	closegraph();
	// 部分 EasyX 版本 closegraph 只停绘不销毁，兜底再销毁一次
	if (easyxHwnd && IsWindow(easyxHwnd)) DestroyWindow(easyxHwnd);

	// 显示启动器并强制同步重绘，避免闪白
	if (g_mainHwnd) {
		g_mode = AppMode::Launcher;
		ShowWindow(g_mainHwnd, SW_SHOW);
		UpdateWindow(g_mainHwnd);
		SetForegroundWindow(g_mainHwnd);
		InvalidateRect(g_mainHwnd, nullptr, FALSE);
	}
}
