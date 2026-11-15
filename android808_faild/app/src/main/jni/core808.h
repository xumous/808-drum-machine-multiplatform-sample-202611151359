/*
 * core808.h —— 808 鼓机共享 DSP / 音序器 / 导出核心
 * ---------------------------------------------------------------------------
 * 设计原则（对应需求中的“硬门槛”）：
 *  1. 所有实时音频计算都发生在 C 层，由各前端（Python/Java/Qt/纯C/Node-WASM）
 *     通过同一个 c808_render() 调用；离线导出复用完全相同的渲染函数。
 *  2. 音频回调内零 malloc/free、零锁、零文件 I/O、零系统调用。
 *     前端 -> 音频线程的参数全部通过单生产者/单消费者无锁环形队列传递。
 *  3. 音序时钟只使用“已渲染采样数”作为权威时间轴，任何 UI 定时器都不参与触发。
 *  4. 工程数据采用双缓冲槽位 + 原子激活索引，加载工程时音频线程不分配、不释放。
 *  5. 自定义快速数学函数（正弦表 / exp2 / tanh），保证桌面、WASM、Android
 *     各平台的 DSP 行为完全一致，也避免依赖 libm 的非确定性。
 *
 * 本文件同时被纯 C、Qt C++、JNI、ctypes、WASM 绑定使用，因此只使用 C99 语法。
 */
#ifndef CORE808_H
#define CORE808_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* 常量                                                                       */
/* ------------------------------------------------------------------------- */
#define C808_TRACKS        16      /* 声部数：底鼓/军鼓/拍手/... 共 16 个           */
#define C808_STEPS         64      /* 每 pattern 最大步数（16/32/64 可选）          */
#define C808_PATTERNS      256     /* pattern 槽位（需求要求 16/64/256 多 pattern） */
#define C808_SONG_ENTRIES  512     /* song 链最大条目                               */
#define C808_VOICES_PER_TRACK 4    /* 每轨 4 复音，16 轨共 64 voice，满足 12 复音    */
#define C808_CMD_RING      16384   /* 无锁命令队列容量（2 的幂）                    */
#define C808_SR_DEFAULT    48000.0f

/* 声部编号（与 GM 鼓映射一致，便于 MIDI 输入/导出） */
enum {
    C808_KICK = 0,
    C808_SNARE,
    C808_CLAP,
    C808_RIM,
    C808_TOM_LO,
    C808_TOM_MID,
    C808_TOM_HI,
    C808_CONGA,
    C808_COWBELL,
    C808_SHAKER,
    C808_TAMB,
    C808_CRASH,
    C808_RIDE,
    C808_HAT_OPEN,
    C808_HAT_CLOSED,
    C808_CLAVE
};

/* 每轨可调参数索引（0..1 浮点，音频线程内做一阶平滑） */
enum {
    C808_P_TUNE = 0,   /* 音高：鼓腔/振荡器基频                        */
    C808_P_DECAY,      /* 衰减：包络时间常数                            */
    C808_P_TONE,       /* 音色：滤波/谐波/点击成分                      */
    C808_P_CHAR,       /* 特性：噪声混合/拍手尾音/金属感等              */
    C808_P_LEVEL,      /* 轨道电平                                      */
    C808_P_PAN,        /* 声像 -1..+1（存储为 0..1，映射到 -1..+1）     */
    C808_P_COUNT
};

/* 全局主参数索引 */
enum {
    C808_M_VOLUME = 0, /* 主音量 0..1         */
    C808_M_DRIVE,      /* 饱和驱动 0..1       */
    C808_M_HP,         /* 主高通截止 0..1     */
    C808_M_LP,         /* 主低通截止 0..1     */
    C808_M_RESO,       /* 滤波共鸣 0..1       */
    C808_M_ACCENT,     /* 全局重音强度 0..1   */
    C808_M_COUNT
};

/* 每步条件触发类型 */
enum {
    C808_COND_NONE = 0,
    C808_COND_1_2,     /* 每 2 次循环触发一次       */
    C808_COND_1_3,
    C808_COND_1_4,
    C808_COND_FILL,    /* 仅第 4 小节（fill）       */
    C808_COND_NOTFILL, /* 非 fill 小节              */
    C808_COND_EVERY_N  /* 每 condN 次循环           */
};

/* 无锁命令类型 */
typedef enum {
    C808_CMD_SET_STEP_ON = 1,
    C808_CMD_SET_STEP_VEL,
    C808_CMD_SET_STEP_ACCENT,
    C808_CMD_SET_STEP_PROB,
    C808_CMD_SET_STEP_RATCHET,
    C808_CMD_SET_STEP_RATE,
    C808_CMD_SET_STEP_COND,
    C808_CMD_SET_STEP_CONDN,
    C808_CMD_SET_STEP_MICRO,
    C808_CMD_SET_TRACK_PARAM,   /* a=track, b=param, f=0..1 */
    C808_CMD_SET_TRACK_LEN,
    C808_CMD_SET_TRACK_MUTE,
    C808_CMD_SET_TRACK_SOLO,
    C808_CMD_SET_MASTER,        /* b=master param */
    C808_CMD_SET_BPM,
    C808_CMD_SET_TIMESIG,       /* a=num, b=den */
    C808_CMD_SET_CLOCK,         /* a=0 内部, 1=MIDI 时钟 */
    C808_CMD_SET_PATTERN_LEN,
    C808_CMD_SET_PATTERN_SWING,
    C808_CMD_SELECT_PATTERN,
    C808_CMD_SET_SONG_ON,
    C808_CMD_SET_SONG_LEN,
    C808_CMD_SET_SONG_ENTRY,    /* a=index, b=pattern, c=repeats */
    C808_CMD_SET_PATTERN_COUNT,
    C808_CMD_CLEAR_PATTERN,
    C808_CMD_CLEAR_TRACK,
    C808_CMD_COPY_PATTERN,      /* a=src, b=dst */
    C808_CMD_TRANSPORT_PLAY,
    C808_CMD_TRANSPORT_STOP,
    C808_CMD_TRANSPORT_STOP_RESET,
    C808_CMD_PANIC,
    C808_CMD_NOTE_ON,           /* a=track, b=velocity, c=accent */
    C808_CMD_SET_TRACK_NOTE,    /* a=track, b=midi note */
    C808_CMD_ACTIVATE_SLOT,     /* a=slot(0/1) 工程双缓冲切换 */
    C808_CMD_RESET_PATTERN,
    C808_CMD_SET_PATTERN_COPY_ALL /* 整工程复制：由前端写非活动槽位后激活 */
} c808_cmd_type;

/* ------------------------------------------------------------------------- */
/* 数据结构                                                                   */
/* ------------------------------------------------------------------------- */
typedef struct {
    uint8_t on[C808_TRACKS][C808_STEPS];
    uint8_t vel[C808_TRACKS][C808_STEPS];     /* 1..127 */
    uint8_t accent[C808_TRACKS][C808_STEPS];  /* 0/1    */
    uint8_t prob[C808_TRACKS][C808_STEPS];    /* 0..100 */
    uint8_t ratchet[C808_TRACKS][C808_STEPS]; /* 1..8   */
    uint8_t rate[C808_TRACKS][C808_STEPS];    /* 10..100，ratchet 间距百分比 */
    uint8_t cond[C808_TRACKS][C808_STEPS];    /* C808_COND_* */
    uint8_t condN[C808_TRACKS][C808_STEPS];   /* 1..8 */
    int16_t micro[C808_TRACKS][C808_STEPS];   /* 相对网格的采样偏移 ±2400 (50ms@48k) */
    uint8_t len;                              /* 1..64 */
    uint8_t swing;                            /* 0..100 */
    uint8_t reserved[2];
} c808_pattern;

typedef struct {
    uint32_t magic;         /* '8','0','8','P' */
    uint32_t version;       /* 当前为 1       */
    char     name[64];
    float    bpm;           /* 40..300 */
    int32_t  tsNum;         /* 拍号分子 1..16 */
    int32_t  tsDen;         /* 拍号分母 1,2,4,8,16 */
    int32_t  currentPattern;
    int32_t  patternCount;
    int32_t  songOn;
    int32_t  songLen;
    struct { uint16_t pat; uint16_t reps; } song[C808_SONG_ENTRIES];

    c808_pattern patterns[C808_PATTERNS];

    float    tune[C808_TRACKS];
    float    decay[C808_TRACKS];
    float    tone[C808_TRACKS];
    float    chara[C808_TRACKS];
    float    level[C808_TRACKS];
    float    pan[C808_TRACKS];
    uint8_t  mute[C808_TRACKS];
    uint8_t  solo[C808_TRACKS];
    uint8_t  trackLen[C808_TRACKS];
    uint8_t  midiNote[C808_TRACKS];

    float    master[C808_M_COUNT];
    int32_t  clockSource;   /* 0=内部, 1=MIDI 时钟 */
} c808_project;

/* 命令结构：固定 24 字节，保持环形队列元素大小稳定 */
typedef struct {
    int32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    float   f;
} c808_cmd;

typedef struct c808_voice c808_voice;

/* 引擎对外不透明；前端只通过 API 访问（Qt/C 可直接包含头文件） */
typedef struct c808_engine c808_engine;

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */
c808_engine* c808_create(double sample_rate); /* UI 线程调用，分配全部内存 */
void         c808_destroy(c808_engine* e);

/* 渲染：音频回调线程调用。outL/outR 为 frames 个 float。零分配、零锁。 */
void c808_render(c808_engine* e, float* outL, float* outR, int frames);
void c808_render_interleaved(c808_engine* e, float* out, int frames);

/* 无锁命令投递（UI 线程）。返回 0 成功，-1 队列满。 */
int  c808_send_cmd(c808_engine* e, const c808_cmd* cmd);
int  c808_set_step_on(c808_engine* e, int pat, int track, int step, int on);
int  c808_set_step_vel(c808_engine* e, int pat, int track, int step, int vel);
int  c808_set_step_accent(c808_engine* e, int pat, int track, int step, int accent);
int  c808_set_step_prob(c808_engine* e, int pat, int track, int step, int prob);
int  c808_set_step_ratchet(c808_engine* e, int pat, int track, int step, int count);
int  c808_set_step_rate(c808_engine* e, int pat, int track, int step, int rate);
int  c808_set_step_cond(c808_engine* e, int pat, int track, int step, int cond, int condN);
int  c808_set_step_micro(c808_engine* e, int pat, int track, int step, int microSamples);
int  c808_set_track_param(c808_engine* e, int track, int param, float value);
int  c808_set_track_len(c808_engine* e, int track, int len);
int  c808_set_mute(c808_engine* e, int track, int mute);
int  c808_set_solo(c808_engine* e, int track, int solo);
int  c808_set_master(c808_engine* e, int param, float value);
int  c808_set_bpm(c808_engine* e, float bpm);
int  c808_set_timesig(c808_engine* e, int num, int den);
int  c808_set_clock_source(c808_engine* e, int midiClock);
int  c808_set_pattern_len(c808_engine* e, int pat, int len);
int  c808_set_pattern_swing(c808_engine* e, int pat, int swing);
int  c808_select_pattern(c808_engine* e, int pat);
int  c808_set_song_on(c808_engine* e, int on);
int  c808_set_song_entry(c808_engine* e, int index, int pat, int reps);
int  c808_set_song_len(c808_engine* e, int len);
int  c808_set_pattern_count(c808_engine* e, int count);
int  c808_clear_pattern(c808_engine* e, int pat);
int  c808_clear_track(c808_engine* e, int pat, int track);
int  c808_copy_pattern(c808_engine* e, int src, int dst);
int  c808_transport_play(c808_engine* e);
int  c808_transport_stop(c808_engine* e);
int  c808_transport_stop_reset(c808_engine* e);
int  c808_note_on(c808_engine* e, int track, int vel, int accent);
int  c808_panic(c808_engine* e);
int  c808_activate_slot(c808_engine* e, int slot);

/* ------------------------------------------------------------------------- */
/* 状态查询（UI 线程读取，全部为原子量或只读快照）                            */
/* ------------------------------------------------------------------------- */
typedef struct {
    int playing;
    int pattern;       /* 当前播放 pattern（song 模式下会变化） */
    int step;          /* 全局步进（0..patternLen-1） */
    int songPos;
    int loopCount;
    int xruns;         /* 前端可回填的欠载计数（c808_report_xrun） */
    float cpuLoad;     /* 0..1，前端回填 */
    float meter[C808_TRACKS];
    float masterL, masterR;
} c808_status;

void c808_get_status(c808_engine* e, c808_status* out);
void c808_report_xrun(c808_engine* e);
void c808_report_cpu(c808_engine* e, float load);

/* 只读访问当前活动工程（用于 UI 绘制；编辑请走命令队列） */
const c808_project* c808_project_ro(c808_engine* e);
/* 跨语言读取接口（Python ctypes / JNI 等无法直接展开结构体时使用） */
enum {
    C808_F_ON = 0, C808_F_VEL, C808_F_ACCENT, C808_F_PROB,
    C808_F_RATCHET, C808_F_RATE, C808_F_COND, C808_F_CONDN, C808_F_MICRO
};
int   c808_get_step(c808_engine* e, int pat, int track, int step, int field);
float c808_get_track_param(c808_engine* e, int track, int param);
int   c808_get_track_len(c808_engine* e, int track);
int   c808_get_track_mute(c808_engine* e, int track);
int   c808_get_track_solo(c808_engine* e, int track);
int   c808_get_track_note(c808_engine* e, int track);
float c808_get_master(c808_engine* e, int param);
float c808_get_bpm(c808_engine* e);
int   c808_get_pattern_len(c808_engine* e, int pat);
int   c808_get_pattern_swing(c808_engine* e, int pat);
int   c808_get_current_pattern(c808_engine* e);
int   c808_get_pattern_count(c808_engine* e);
int   c808_get_song_on(c808_engine* e);
int   c808_get_song_len(c808_engine* e);
int   c808_get_song_entry(c808_engine* e, int index, int* pat, int* reps);
int   c808_get_timesig(c808_engine* e, int* num, int* den);
int   c808_get_clock_source(c808_engine* e);
/* 一次性快照当前 pattern，供 Java/Python 等前端每帧批量读取（无分配）。
 * meta[0..6] = len,swing,currentPattern,songOn,patternCount,tsNum,tsDen
 * params[0]=bpm, params[1..6]=master[0..5], params[7+t*6+p]=track param
 * steps  = 16*64*10 字节（8 字节字段 + int16 micro）
 * flags  = 16*4 字节（mute,solo,trackLen,midiNote） */
void  c808_snapshot(c808_engine* e, int pat, int32_t* meta, float* params,
                    uint8_t* steps, uint8_t* flags);
/* 可写访问“非活动槽位”，用于加载工程/整工程替换；写入后调用 activate_slot */
c808_project* c808_project_shadow(c808_engine* e);
/* 激活当前非活动槽位（加载工程后使用） */
int c808_activate_shadow(c808_engine* e);

/* 默认工程：内置一段演示 pattern，全部音色为模拟合成参数 */
void c808_init_default_project(c808_project* p);

/* ------------------------------------------------------------------------- */
/* 导出与 I/O（UI/工作线程调用，内部创建独立引擎，不干扰实时实例）            */
/* ------------------------------------------------------------------------- */
/* mode: 0=pattern, 1=song; loops: pattern 循环次数（song 模式忽略） */
int c808_export_wav(c808_engine* e, const char* path, int mode, int loops,
                    int includeTailSeconds, int stems);
int c808_export_midi(c808_engine* e, const char* path, int mode, int loops);

int c808_save_project(c808_engine* e, const char* path);
int c808_load_project(c808_engine* e, const char* path); /* 写入 shadow 后激活 */

/* MIDI 输入（Windows: winmm；其他平台空实现）。事件进入无锁队列。 */
int  c808_midi_start(c808_engine* e, int deviceIndex);
void c808_midi_stop(c808_engine* e);
int  c808_midi_device_count(void);
int  c808_midi_device_name(int index, char* out, int cap);
/* 供 Android/Web MIDI 等外部 MIDI 实现把事件推入音频线程（无锁） */
int  c808_push_midi(c808_engine* e, int type, int a, int b, int c);

/* 离线测试/诊断：返回本轮渲染中触发的 note 数（用于采样级时序验证） */
uint64_t c808_trigger_count(c808_engine* e);

#ifdef __cplusplus
}
#endif

#endif /* CORE808_H */
