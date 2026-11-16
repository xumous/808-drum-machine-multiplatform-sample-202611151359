/* ==========================================================================
 *  xu808.h —— XU-808 纯 C 版公共头文件
 *
 *  徐某写这份头文件时，把“跨语言一致”放在第一位：
 *  结构体字段的顺序、含义、取值范围，全部与 Python/C#/Java 版一一对应，
 *  这样同一个 .xu808 工程文件（JSON）可以在各版本之间迁移。
 *
 *  编译环境：MinGW-w64 (GCC) + Win32 API，零第三方库。
 * ========================================================================== */
#ifndef XU808_H
#define XU808_H

#include <windows.h>

/* ---- 全局音频常量（不可改，所有版本一致） ---- */
#define XU_SR            48000      /* 采样率 48kHz                    */
#define XU_PADS          16         /* 声部数                          */
#define XU_MAX_VOICES    4          /* 每声部复音数                    */
#define XU_BLOCK         256        /* 回调块大小：5.33ms              */
#define XU_STEPS_MAX     64         /* 单个 pattern 的最大步数          */
#define XU_PATTERNS      16         /* 工程内 pattern 数量              */
#define XU_MAX_EVENTS    4096       /* 事件队列容量（够用且不占内存）    */

/* ---- 条件触发枚举（数值即存储值，规格书第 4 节） ---- */
#define XU_COND_ALWAYS   0
#define XU_COND_FILL     1
#define XU_COND_NOTFILL  2
#define XU_COND_EVERY2   3
#define XU_COND_EVERY4   4

/* ---- 声部代号与 MIDI 音符；顺序就是编号，绝不能重排 ---- */
extern const char* XU_CODES[XU_PADS];
extern const wchar_t* XU_NAMES_CN[XU_PADS];
extern const int XU_MIDI_NOTES[XU_PADS];

/* --------------------------------------------------------------------------
 *  一个格子的全部属性
 *  用 unsigned char 存布尔/小整数，是为了让 16×64 的网格只占约 8KB，
 *  复制 pattern、写文件都非常快。
 * -------------------------------------------------------------------------- */
typedef struct
{
    unsigned char on; /* 是否启用                  */
    unsigned char velocity; /* 力度 1..127               */
    unsigned char accent; /* 重音 0/1                  */
    unsigned char prob; /* 概率 0..100               */
    short micro; /* 微时偏移（样本，-2400..2400）*/
    unsigned char ratchet; /* 滚奏 1..8                 */
    unsigned char cond; /* 条件触发                  */
} XuStep;

/* 一个 Pattern：16 轨 × 64 步，另加每轨独立长度（polymeter） */
typedef struct
{
    char name[32];
    int steps; /* 1..64 */
    int fill; /* 是否过门循环 */
    XuStep data[XU_PADS][XU_STEPS_MAX];
    unsigned char trackLen[XU_PADS]; /* 1..64，每轨循环长度 */
} XuPattern;

/* 每声部参数：0..1，除 level/pan 外都按规格书映射 */
typedef struct
{
    float tune;
    float decay;
    float tone;
    float snappy;
    float tail;
    float level;
    float pan;
    int mute;
    int solo;
} XuPadParams;

/* 主效果与全局设置 */
typedef struct
{
    float volume;
    float drive;
    float lpCutoff;
    float lpQ;
    float hpCutoff;
    float hpQ;
    int lpOn;
    int hpOn;
    float bpm;
    float swing;
    int beatsPerBar;
    int beatUnit;
    int songMode;
} XuMaster;

/* 歌曲链的一节 */
typedef struct
{
    int pattern;
    int repeats;
} XuSongEntry;

/* 顶层工程 */
typedef struct
{
    XuPattern patterns[XU_PATTERNS];
    XuPadParams pads[XU_PADS];
    XuMaster master;
    XuSongEntry song[64];
    int songCount;
    int current;
    unsigned int seed;
    char name[64];
} XuProject;

/* --------------------------------------------------------------------------
 *  RBJ 双二阶滤波器
 *  zi 用两个 double 保存延迟线；必须跨块保留，否则每块都会“咔”一声。
 * -------------------------------------------------------------------------- */
typedef struct
{
    double b0, b1, b2, a1, a2; /* 系数（a0 已归一化为 1） */
    double z1, z2; /* 状态                    */
    int kind; /* 0=直通 1=LP 2=HP 3=BP   */
    double fc, q; /* 上次参数，用于避免重复计算 */
} XuBiquad;

/* --------------------------------------------------------------------------
 *  一个鼓槌（voice）
 *  噪声池 8192 个 float = 32KB，构造时一次性填好，回调内只读不分配。
 * -------------------------------------------------------------------------- */
typedef struct
{
    int pad;
    int active;
    long long start; /* 触发绝对样本            */
    long long stop; /* 强制结束时刻            */
    float gain; /* 力度增益（含重音）      */
    int accent; /* 重音标志（影响音色）    */
    int fadeLen; /* 掐断淡出长度            */
    float noise[8192]; /* 预生成噪声池            */
    int npos; /* 噪声读取指针            */
    XuBiquad bp, hp, bp2; /* 三个滤波器              */
} XuVoice;

/* 内部事件：绝对样本时刻 + 声部 + 力度 + 重音 */
typedef struct
{
    long long t;
    int pad;
    float vel;
    int accent;
} XuEvent;

/* --------------------------------------------------------------------------
 *  引擎：音序器 + 混音 + 主效果 + 统计
 * -------------------------------------------------------------------------- */
typedef struct
{
    XuProject* proj;
    XuVoice voices[XU_PADS][XU_MAX_VOICES];
    volatile int playing;
    long long samplePos; /* 下一块的绝对起点        */
    long long nextStepAbs;
    int stepIndex;
    long long globalStep;
    int cycleCount;
    int fillNow;
    int pendingPattern;
    int songIndex;
    int songRepeat;
    unsigned int rng;
    /* 事件队列：pending 是音序器的，manual 是鼠标/键盘的 */
    XuEvent pending[XU_MAX_EVENTS];
    int pendingCount;
    XuEvent manual[XU_MAX_EVENTS];
    int manualCount;
    XuBiquad lpL, lpR, hpL, hpR;
    volatile float meterL, meterR;
    volatile int xruns;
    volatile float cpu;
    int clockExternal;
    int clockPulse;
    int soloOverride; /* >=0 时离线分轨只让该轨出声 */
} XuEngine;

/* ---- dsp.c ---- */
void xu_biquad_lp(XuBiquad* f, double fc, double q);
void xu_biquad_hp(XuBiquad* f, double fc, double q);
void xu_biquad_bp(XuBiquad* f, double fc, double q);
double xu_biquad_run(XuBiquad* f, double x);
void xu_voice_trigger(XuVoice* v, long long when, float gain, int accent);
void xu_voice_kill(XuVoice* v, long long when, int fade);
void xu_voice_render(XuVoice* v, float* L, float* R, int n,
                     long long b0, long long b1, const XuPadParams* p);

/* ---- engine.c ---- */
void xu_project_default(XuProject* p);
void xu_project_demo(XuPattern* pat);
void xu_engine_init(XuEngine* e, XuProject* p);
void xu_engine_play(XuEngine* e);
void xu_engine_stop(XuEngine* e);
void xu_engine_render(XuEngine* e, int n, float* L, float* R);
void xu_engine_trigger_pad(XuEngine* e, int pad, int vel, int accent);
void xu_engine_request_pattern(XuEngine* e, int idx);
void xu_engine_external_tick(XuEngine* e);

int xu_audio_start(XuEngine* e);
void xu_audio_stop(void);
int xu_audio_running(void);

/* 离线导出；成功返回 1 */
int xu_export_wav(XuProject* p, const wchar_t* path, int song, int soloPad);
int xu_export_midi(XuProject* p, const wchar_t* path, int song);

/* 工程存档（简化文本格式，字段与 JSON 版本一一对应） */
int xu_project_save(XuProject* p, const wchar_t* path);
int xu_project_load(XuProject* p, const wchar_t* path);

/* ---- ui.c ---- */
int xu_ui_run(HINSTANCE hInst, XuProject* p);

#endif /* XU808_H */