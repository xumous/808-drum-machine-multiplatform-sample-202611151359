;
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
;
xu808
.
inc — — XU
-
808
 ASM 版公共定义（结构偏移、 常量、 宏）;
;
署名：徐某。
;
  这个文件把 C 版 xu808
.
h 的结构体布局逐字段搬到汇编：;
  偏移量必须与 C 版一致，DSP 与音序器才能复用同一套公式。
;
  所有浮点运算使用标量 SSE2 双精度；输出前再转 float32
/
16bit。
;
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==

;
--
--
--
--
--
--
--
--
全局音频常量（与规格书一致）
--
--
--
--
--
--
--
--
%
define XU_SR
48000
%
define XU_PADS
16
%
define XU_MAX_VOICES
4
%
define XU_BLOCK
256
%
define XU_STEPS_MAX
64
%
define XU_PATTERNS
16
%
define XU_MAX_EVENTS
4096

;
--
--
--
--
--
--
--
--
条件触发
--
--
--
--
--
--
--
--
%
define COND_ALWAYS
0
%
define COND_FILL
1
%
define COND_NOTFILL
2
%
define COND_EVERY2
3
%
define COND_EVERY4
4

;
--
--
--
--
--
--
--
--
步进结构（8 字节）
--
--
--
--
--
--
--
--
%
define ST_ON
0
%
define ST_VEL
1
%
define ST_ACC
2
%
define ST_PROB
3
%
define ST_MICRO
4
%
define ST_RATCHET
6
%
define ST_COND
7
%
define ST_SIZE
8

;
--
--
--
--
--
--
--
--
Pattern 结构
--
--
--
--
--
--
--
--
%
define PAT_NAME
0 ;
char [32]
%
define PAT_STEPS
32 ;
int
%
define PAT_FILL
36 ;
int
%
define PAT_DATA
40 ;
16
*
64
*
8
=
8192
%
define PAT_TRACKLEN
8232 ;
u8 [16]
%
define PAT_SIZE
8248

;
--
--
--
--
--
--
--
--
声部参数（40 字节）
--
--
--
--
--
--
--
--
%
define PP_TUNE
0
%
define PP_DECAY
4
%
define PP_TONE
8
%
define PP_SNAPPY
12
%
define PP_TAIL
16
%
define PP_LEVEL
20
%
define PP_PAN
24
%
define PP_MUTE
28
%
define PP_SOLO
32
%
define PP_SIZE
40

;
--
--
--
--
--
--
--
--
主控参数（56 字节）
--
--
--
--
--
--
--
--
%
define MS_VOLUME
0
%
define MS_DRIVE
4
%
define MS_LP
8
%
define MS_LPQ
12
%
define MS_HP
16
%
define MS_HPQ
20
%
define MS_LPON
24
%
define MS_HPON
28
%
define MS_BPM
32
%
define MS_SWING
36
%
define MS_BEATS
40
%
define MS_UNIT
44
%
define MS_SONGMODE
48
%
define MS_SIZE
56

;
--
--
--
--
--
--
--
--
工程结构
--
--
--
--
--
--
--
--
%
define PJ_PATTERNS
0
%
define PJ_PADS
131968
%
define PJ_MASTER
132608
%
define PJ_SONG
132664
%
define PJ_SONGCOUNT
133176
%
define PJ_CURRENT
133180
%
define PJ_SEED
133184
%
define PJ_NAME
133188
%
define PJ_SIZE
133252

;
--
--
--
--
--
--
--
--
 RBJ 双二阶（80 字节）
--
--
--
--
--
--
--
--
%
define BQ_B0
0
%
define BQ_B1
8
%
define BQ_B2
16
%
define BQ_A1
24
%
define BQ_A2
32
%
define BQ_Z1
40
%
define BQ_Z2
48
%
define BQ_KIND
56
%
define BQ_FC
64
%
define BQ_Q
72
%
define BQ_SIZE
80

;
--
--
--
--
--
--
--
--
Voice（33072 字节）
--
--
--
--
--
--
--
--
%
define V_PAD
0
%
define V_ACTIVE
4
%
define V_START
8
%
define V_STOP
16
%
define V_GAIN
24 ;
double
%
define V_ACCENT
32
%
define V_FADELEN
36
%
define V_NPOS
40
%
define V_NOISE
64 ;
float [8192] =
32768
%
define V_BP
32832
%
define V_HP
32912
%
define V_BP2
32992
%
define V_SIZE
33072

;
--
--
--
--
--
--
--
--
事件（32 字节）
--
--
--
--
--
--
--
--
%
define E_T
0
%
define E_PAD
8
%
define E_VEL
16 ;
double
%
define E_ACCENT
24
%
define E_SIZE
32

;
--
--
--
--
--
--
--
--
引擎（262592 字节）
--
--
--
--
--
--
--
--
%
define EN_PROJ
0
%
define EN_PLAYING
8
%
define EN_SAMPLEPOS
16
%
define EN_NEXTSTEP
24
%
define EN_STEPINDEX
32
%
define EN_GLOBALSTEP
40
%
define EN_CYCLECOUNT
48
%
define EN_FILLNOW
52
%
define EN_PENDINGPAT
56
%
define EN_SONGINDEX
60
%
define EN_SONGREPEAT
64
%
define EN_RNG
68
%
define EN_PENDINGCOUNT
72
%
define EN_MANUALCOUNT
76
%
define EN_PENDING
80 ;
4096
*
32
%
define EN_MANUAL
131152 ;
4096
*
32
%
define EN_LPL
262224
%
define EN_LPR
262304
%
define EN_HPL
262384
%
define EN_HPR
262464
%
define EN_METERL
262544
%
define EN_METERR
262548
%
define EN_XRUNS
262552
%
define EN_CPU
262556
%
define EN_CLOCKEXT
262560
%
define EN_CLOCKPULSE
262564
%
define EN_SOLO
262568
%
define EN_VOICES
262576
%
define EN_EVBUF
262584
%
define EN_SIZE
262592

;
--
--
--
--
--
--
--
--
waveOut 音频状态
--
--
--
--
--
--
--
--
%
define AU_HWO
0
%
define AU_FMT
8 ;
WAVEFORMATEX
18
字节
%
define AU_HDR
32 ;
WAVEHDR [4]
192
字节
%
define AU_DATA
224 ;
short [4]
[512]
4096
字节
%
define AU_NEXT
4320
%
define AU_RUNNING
4324
%
define AU_ENG
4328
%
define AU_FREQ
4336
%
define AU_SIZE
4352

%
define WH_LPDATA
0
%
define WH_LEN
8
%
define WH_REC
12
%
define WH_USER
16
%
define WH_FLAGS
24
%
define WH_LOOPS
28
%
define WH_NEXT
32
%
define WH_RES
40
%
define WH_SIZE
48

;
--
--
--
--
--
--
--
--
函数序言
/
收尾宏
--
--
--
--
--
--
--
--
;
进入时 rsp
%
16
==
8；push
rbp 后为
0；sub
N（N 为
16
的倍数）保持
16
对齐。
;
被调用者保存寄存器统一存到 [rbp-8..-56]
，正文局部变量从 [rbp-64]
开始。

%
macro PROLOG
1
push rbp
mov rbp, rsp
sub rsp, 
%
1
mov [rbp-8]
,
rbx
    mov[rbp - 16], rsi
mov [rbp-24]
,
rdi
    mov[rbp - 32], r12
mov [rbp-40]
,
r13
    mov[rbp - 48], r14
mov [rbp-56]
,
r15
%
endmacro
%
macro EPILOG
0
mov rbx, [rbp-8]
mov rsi, [rbp-16]
mov rdi, [rbp-24]
mov r12, [rbp-32]
mov r13, [rbp-40]
mov r14, [rbp-48]
mov r15, [rbp-56]
leave
ret
%
endmacro;
读取 float 参数并转成 double：LDF xmmreg, base, off
%
macro LDF
3
movss
%
1
,
[%2+%3]
cvtss2sd
%
1
,
%
1
%
endmacro;
文本宏：TextOutA(hdc, x, y, str, len)，要求当前帧 rsp 已对齐
%
macro TEXTA
5
mov rcx, 
%
1
mov edx, 
%
2
mov r8d, 
%
3
lea r9, [rel %4]
mov qword[rsp + 32], 
%
5
call TextOutA
%
endmacro;
宽文本宏：TextOutW(hdc, x, y, str, len)
%
macro TEXTW
5
mov rcx, 
%
1
mov edx, 
%
2
mov r8d, 
%
3
lea r9, [rel %4]
mov qword[rsp + 32], 
%
5
call TextOutW
%
endmacro;
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
;
常量池（每个
.
asm
各包含一份，标签是对象内局部符号）;
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
==
section
.
rdata
align
16

c_zero : dq
0.0
c_one : dq
1.0
c_neg_one : dq
-
1.0
c_two : dq
2.0
c_neg_two : dq
-
2.0
c_half : dq
0.5
c_pi : dq
3.14159265358979323846
c_two_pi : dq
6.28318530717958647692
c_two_pi_lo : dq
2.4492935982947064e-16
c_half_pi : dq
1.57079632679489661923
c_inv_two_pi : dq
0.15915494309189533577
c_log2e : dq
1.44269504088896340736
c_ln2 : dq
0.69314718055994530942
c_absmask : dq
0x7FFFFFFFFFFFFFFF
c_signmask : dq
0x8000000000000000

;

双精度钳位边界
c_neg80: dq

-
80.0
c_pos80 : dq
80.0
c_neg15 : dq
-
15.0
c_pos15 : dq
15.0
c_20 : dq
20.0
c_10 : dq
10.0
c_23520 : dq
23520.0
c_01 : dq
0.1
c_03 : dq
0.3
c_30 : dq
30.0
c_neg_half : dq
-
0.5
c_001 : dq
0.001
c_neg001 : dq
-
0.001
c_two_pi_over_sr : dq
0.00013089969389957470

;
采样率
/
节拍
c_sr: dq

48000.0
c_inv_sr : dq
0.000020833333333333333
c_720000 : dq
720000.0
c_192000 : dq
192000.0
c_6000 : dq
6000.0

;

通用比例
c_42: dq

0.42
c_092 : dq
0.92
c_118 : dq
1.18
c_125 : dq
1.25
c_130 : dq
1.30
c_002 : dq
0.02
c_100 : dq
100.0
c_127 : dq
127.0
c_8388608 : dq
8388608.0
c_inv8388608 : dq
0.00000011920928955078125
c_32767 : dq
32767.0

;
 正弦多项式系数（Taylor 到 x
^
11）
c_sin2 : dq
0.16666666666666666667
c_sin4 : dq
0.00833333333333333333
c_sin6 : dq
0.00019841269841269841
c_sin8 : dq
0.00000275573192239859
c_sin10 : dq
0.00000002505210838544

;
 exp 多项式系数（Taylor 到 u
^
7）
c_e1 : dq
0.14285714285714285714
c_e2 : dq
0.04166666666666666667
c_e3 : dq
0.00833333333333333333
c_e4 : dq
0.00138888888888888889
c_e5 : dq
0.00019841269841269841
c_e6 : dq
0.00002480158730158730
c_e7 : dq
0.00000275573192239859

;
--
--
--
--
--
--
--
--
合成公式常量
--
--
--
--
--
--
--
--
c_40 : dq
40.0
c_80 : dq
80.0
c_110 : dq
110.0
c_150 : dq
150.0
c_170 : dq
170.0
c_220 : dq
220.0
c_300 : dq
300.0
c_320 : dq
320.0
c_400 : dq
400.0
c_700 : dq
700.0
c_1200 : dq
1200.0
c_1400 : dq
1400.0
c_1800 : dq
1800.0
c_2000 : dq
2000.0
c_2400 : dq
2400.0
c_2500 : dq
2500.0
c_3000 : dq
3000.0
c_4000 : dq
4000.0
c_5000 : dq
5000.0
c_5200 : dq
5200.0
c_7600 : dq
7600.0
c_10000 : dq
10000.0
c_2600 : dq
2600.0
c_3600 : dq
3600.0
c_12000 : dq
12000.0
c_1_6 : dq
1.6
c_1_9 : dq
1.9
c_1_15 : dq
1.15
c_1_2 : dq
1.2
c_1_48 : dq
1.48
c_1_18 : dq
1.18
c_1_78 : dq
1.78
c_0_22 : dq
0.22
c_0_34 : dq
0.34
c_0_55 : dq
0.55
c_0_7 : dq
0.7
c_0_25 : dq
0.25
c_0_9 : dq
0.9
c_0_8 : dq
0.8
c_0_85 : dq
0.85
c_0_018 : dq
0.018
c_0_012 : dq
0.012
c_0_12 : dq
0.12
c_0_0035 : dq
0.0035
c_0_06 : dq
0.06
c_0_32 : dq
0.32
c_0_055 : dq
0.055
c_0_14 : dq
0.14
c_0_075 : dq
0.075
c_0_16 : dq
0.16
c_0_15 : dq
0.15
c_0_75 : dq
0.75
c_0_04 : dq
0.04
c_0_02 : dq
0.02
c_0_035 : dq
0.035
c_0_020 : dq
0.020
c_0_004 : dq
0.004
c_0_05 : dq
0.05
c_2_2 : dq
2.2
c_0_35 : dq
0.35
c_0_1 : dq
0.1
c_0_009 : dq
0.009
c_0_011 : dq
0.011
c_0_0015 : dq
0.0015
c_0_028 : dq
0.028
c_0_20 : dq
0.20
c_0_65 : dq
0.65
c_0_175 : dq
1.75
c_1_4 : dq
1.4
c_1_1 : dq
1.1
c_1_35 : dq
1.35
c_0_72 : dq
0.72
c_0_6 : dq
0.6
c_0_5 : dq
0.5
c_6 : dq
6.0
c_8 : dq
8.0
c_1_001 : dq
1.001
c_3800 : dq
3800.0
c_1700 : dq
1700.0
c_800 : dq
800.0
c_1000 : dq
1000.0
c_1400b : dq
1400.0

;
镲族
6
个非谐和比例
align
8
metal_ratios :
    dq

1.0
,
1.3423
,
1.2312
,
1.6532
,
1.9523
,
2.1523

;
嗵鼓
/
康加基频表（pad
2..7）
tom_base :
    dq

80.0
,
120.0
,
170.0
,
220.0
,
300.0
,
400.0

;
ratchet 力度衰减
0.92
^
i
align
8
ratchet_gain :
    dq

1.0
,
0.92
,
0.8464
,
0.778688
,
0.71639296
,
0.6590815232
dq
0.606355001344
,
0.55784660123648

;
16
字节对齐的绝对值掩码（andpd 内存操作数必须
16
字节对齐）
align
16
c_abs16 :
    dq

0x7FFFFFFFFFFFFFFF
,
0x7FFFFFFFFFFFFFFF

;
--
--
--
--
--
--
--
--
UI 颜色（COLORREF = 0x00BBGGRR）----------------
       % define COL_BG
0x00121212
%
define COL_PANEL
0x001C1C1C
%
define COL_ORANGE
0x00187AFF
%
define COL_GRID
0x002E2E2E
%
define COL_AMBER
0x0066D1FF
%
define COL_CYAN
0x00F0C94C
%
define COL_TEXT
0x00E0E0E0
%
define COL_DIM
0x00808080
%
define COL_STEPOFF
0x00252525
%
define COL_GREEN
0x0060D060