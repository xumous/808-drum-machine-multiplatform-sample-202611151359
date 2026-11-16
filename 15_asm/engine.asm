; ==========================================================================
;  engine.asm —— 工程 / 音序器 / 混音 / waveOut / 离线导出（x86-64 NASM）
;
;  署名：徐某。
;  与 03_c_win32/engine.c 对齐的要点：
;    · samplePos(int64) 是唯一时钟，所有事件先展开成绝对样本时刻；
;    · swing / micro / ratchet / probability / condition 全部按样本落位；
;    · pattern 切换只在循环边界（下一步）生效；
;    · mute/solo 在混音阶段阻断，voice 仍照常衰减，恢复时不会突然冒音；
;    · 导出 WAV 复用同一套 render，只把时钟换成固定块推进。
;
;  音频线程纪律：render 内不分配、不加锁、不做文件 I/O。
;  栈约定：所有会 call 的函数都留出 32 字节影子空间（PROLOG >= 96），
;  局部变量一律不进入 [rsp, rsp+64)，避免与影子空间/第 5 参数槽重叠。
; ==========================================================================
%include "xu808.inc"

default rel

extern xu_bq_lp
extern xu_bq_hp
extern xu_bq_run
extern xu_dtanh
extern xu_voice_render

extern CreateFileW
extern WriteFile
extern CloseHandle
extern waveOutOpen
extern waveOutWrite
extern waveOutPrepareHeader
extern waveOutUnprepareHeader
extern waveOutReset
extern waveOutClose
extern Sleep

; ---------------- 全局数据 ----------------
section .bss
alignb 64
global g_project
g_project:      resb PJ_SIZE + 64
alignb 64
global g_engine
g_engine:       resb EN_SIZE
alignb 64
global g_exp_engine
g_exp_engine:   resb EN_SIZE
alignb 64
global g_voices
g_voices:       resb V_SIZE * 64
alignb 64
global g_exp_voices
g_exp_voices:   resb V_SIZE * 64
alignb 64
g_evbuf:        resb E_SIZE * XU_MAX_EVENTS
alignb 64
g_L:            resb XU_BLOCK * 4
g_R:            resb XU_BLOCK * 4
g_expL:         resb XU_BLOCK * 4
g_expR:         resb XU_BLOCK * 4
g_out16:        resb XU_BLOCK * 4
alignb 64
g_audio:        resb AU_SIZE
g_written:      resb 16

section .rdata
s_projname:     db "XuMou Xu808 Project", 0
s_pattern:      db "PATTERN ", 0

align 4
ff_neg1:        dd -1.0
ff_pos1:        dd 1.0
ff_32767:       dd 32767.0
ff_one:         dd 1.0
ff_zero:        dd 0.0
ff_eight:       dd 8.0
ff_1_001:       dd 1.001
ff_0_75:        dd 0.75

; 取绝对值掩码：必须按 **float** 的粒度（每个 32 位元素 0x7FFFFFFF）。
; xu808.inc 里的 c_abs16 是给 andpd（double）用的，低 32 位为 0xFFFFFFFF，
; 用 andps 配它等于不求绝对值，负半周会被 x/(1+x) 算飞。
align 16
ff_abs_ps:      dd 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF

section .text

; --------------------------------------------------------------------------
;  小工具：字符串复制（rdi=dst, rsi=src）
; --------------------------------------------------------------------------
xu_strcpy:
    lodsb
    stosb
    test    al, al
    jnz     xu_strcpy
    ret

; 小工具：清零（rdi=dst, rcx=len）
xu_bzero:
    xor     eax, eax
    rep     stosb
    ret

; ==========================================================================
;  xu_voice_trigger / xu_voice_kill
; ==========================================================================
global xu_voice_trigger
; rcx = voice, rdx = when, xmm0 = gain(double), r8d = accent
xu_voice_trigger:
    mov     dword [rcx+V_ACTIVE], 1
    mov     [rcx+V_START], rdx
    mov     rax, 0x7fffffffffffffff
    mov     [rcx+V_STOP], rax
    movsd   [rcx+V_GAIN], xmm0
    mov     [rcx+V_ACCENT], r8d
    mov     dword [rcx+V_FADELEN], 64
    mov     dword [rcx+V_NPOS], 0
    xor     eax, eax
    mov     [rcx+V_BP+BQ_Z1], rax
    mov     [rcx+V_BP+BQ_Z2], rax
    mov     [rcx+V_HP+BQ_Z1], rax
    mov     [rcx+V_HP+BQ_Z2], rax
    mov     [rcx+V_BP2+BQ_Z1], rax
    mov     [rcx+V_BP2+BQ_Z2], rax
    ret

global xu_voice_kill
; rcx = voice, rdx = when, r8d = fade
xu_voice_kill:
    cmp     dword [rcx+V_ACTIVE], 0
    je      .ret
    test    r8d, r8d
    jg      .ok
    mov     r8d, 1
.ok:
    movsxd  r8, r8d
    mov     rax, rdx
    add     rax, r8
    cmp     rax, [rcx+V_STOP]
    jge     .nostop
    mov     [rcx+V_STOP], rax
.nostop:
    mov     [rcx+V_FADELEN], r8d
.ret:
    ret

; ==========================================================================
;  xu_project_demo —— 演示律动（与 C 版逐格一致）
;  rcx = XuPattern*
; ==========================================================================
%macro SETCELL 3
    mov     eax, %1
    shl     eax, 6
    add     eax, %2
    shl     eax, 3
    movsxd  rax, eax
    lea     rdx, [r12+PAT_DATA]
    add     rdx, rax
    mov     byte [rdx+ST_ON], 1
    mov     byte [rdx+ST_VEL], %3
%endmacro

global xu_project_demo
xu_project_demo:
    PROLOG  96
    mov     r12, rcx
    ; 所有格子先给默认力度/概率/ratchet
    xor     r13d, r13d
.pad:
    xor     r14d, r14d
.step:
    mov     eax, r13d
    shl     eax, 6
    add     eax, r14d
    shl     eax, 3
    movsxd  rax, eax
    lea     rdx, [r12+PAT_DATA]
    add     rdx, rax
    mov     byte [rdx+ST_VEL], 100
    mov     byte [rdx+ST_PROB], 100
    mov     byte [rdx+ST_RATCHET], 1
    inc     r14d
    cmp     r14d, 64
    jb      .step
    inc     r13d
    cmp     r13d, 16
    jb      .pad
    mov     dword [r12+PAT_STEPS], 16
    ; 底鼓：正拍/反拍 + 第 15 步三连滚奏
    SETCELL 0, 0, 118
    mov     byte [r12+PAT_DATA+0*8+ST_ACC], 1
    SETCELL 0, 8, 105
    SETCELL 0, 14, 90
    SETCELL 0, 15, 88
    mov     byte [r12+PAT_DATA+15*8+ST_RATCHET], 3
    ; 军鼓：2、4 拍
    SETCELL 1, 4, 110
    SETCELL 1, 12, 110
    mov     byte [r12+PAT_DATA+64*8+12*8+ST_ACC], 1
    ; 闭镲：八分音符
    xor     r14d, r14d
.hh:
    test    r14d, 1
    jnz     .hh_next
    SETCELL 15, r14d, 84
    mov     eax, r14d
    and     eax, 3
    jnz     .hh_next
    mov     byte [r12+PAT_DATA+15*64*8+r14*8+ST_VEL], 96
.hh_next:
    inc     r14d
    cmp     r14d, 16
    jb      .hh
    ; 开镲 / 拍手 / 沙锤
    SETCELL 14, 14, 92
    SETCELL 10, 12, 80
    mov     r14d, 1
.shaker:
    SETCELL 11, r14d, 42
    add     r14d, 2
    cmp     r14d, 16
    jb      .shaker
    EPILOG

; ==========================================================================
;  xu_project_default —— 默认工程
;  rcx = XuProject*
; ==========================================================================
global xu_project_default
xu_project_default:
    PROLOG  96
    mov     r12, rcx
    mov     rdi, r12
    mov     rcx, PJ_SIZE
    call    xu_bzero
    lea     rdi, [r12+PJ_NAME]
    lea     rsi, [rel s_projname]
    call    xu_strcpy
    mov     dword [r12+PJ_SEED], 0x808808
    mov     dword [r12+PJ_CURRENT], 0
    ; ---- 16 个声部默认参数 ----
    xor     r13d, r13d
.pad_loop:
    imul    r14, r13, PP_SIZE
    add     r14, r12
    add     r14, PJ_PADS
    mov     dword [r14+PP_TUNE],   0x3F000000    ; 0.50
    mov     dword [r14+PP_DECAY],  0x3F000000    ; 0.50
    mov     dword [r14+PP_TONE],   0x3F000000    ; 0.50
    mov     dword [r14+PP_SNAPPY], 0x3F0CCCCD    ; 0.55
    mov     dword [r14+PP_TAIL],   0x3EE66666    ; 0.45
    mov     dword [r14+PP_LEVEL],  0x3F400000    ; 0.75
    mov     dword [r14+PP_PAN],    0x3F000000    ; 0.50
    mov     dword [r14+PP_MUTE],   0
    mov     dword [r14+PP_SOLO],   0
    inc     r13d
    cmp     r13d, 16
    jb      .pad_loop
    mov     dword [r12+PJ_PADS+0*PP_SIZE+PP_DECAY], 0x3F0CCCCD  ; 底鼓 decay 0.55
    mov     dword [r12+PJ_PADS+0*PP_SIZE+PP_TONE],  0x3EE66666  ; 底鼓 tone 0.45
    mov     dword [r12+PJ_PADS+1*PP_SIZE+PP_DECAY], 0x3EE66666  ; 军鼓 decay 0.45
    ; ---- 16 个 pattern ----
    xor     r13d, r13d
.pat_loop:
    imul    r14, r13, PAT_SIZE
    add     r14, r12
    add     r14, PJ_PATTERNS
    mov     rdi, r14
    mov     rcx, PAT_SIZE
    call    xu_bzero
    lea     rdi, [r14+PAT_NAME]
    lea     rsi, [rel s_pattern]
    call    xu_strcpy
    ; 名字尾号 1..16
    lea     rdi, [r14+PAT_NAME+8]
    lea     eax, [r13+1]
    cmp     eax, 10
    jl      .one_digit
    mov     ecx, eax
    sub     ecx, 10
    mov     al, '1'
    stosb
    mov     eax, ecx
    add     al, '0'
    stosb
    mov     byte [rdi], 0
    jmp     .name_done
.one_digit:
    add     al, '0'
    stosb
    mov     byte [rdi], 0
.name_done:
    mov     dword [r14+PAT_STEPS], 16
    mov     dword [r14+PAT_FILL], 0
    mov     rax, 0x1010101010101010
    mov     [r14+PAT_TRACKLEN], rax
    mov     [r14+PAT_TRACKLEN+8], rax
    ; 每格默认 vel=100 / prob=100 / ratchet=1
    xor     r15d, r15d
.step_loop:
    mov     eax, r15d
    shl     eax, 3
    movsxd  rax, eax
    lea     rdx, [r14+PAT_DATA]
    add     rdx, rax
    mov     byte [rdx+ST_VEL], 100
    mov     byte [rdx+ST_PROB], 100
    mov     byte [rdx+ST_RATCHET], 1
    inc     r15d
    cmp     r15d, 1024
    jb      .step_loop
    inc     r13d
    cmp     r13d, 16
    jb      .pat_loop
    ; 第一个 pattern 放演示律动
    lea     rcx, [r12+PJ_PATTERNS]
    call    xu_project_demo
    ; ---- 主控 ----
    mov     dword [r12+PJ_MASTER+MS_VOLUME], 0x3F4CCCCD  ; 0.80
    mov     dword [r12+PJ_MASTER+MS_DRIVE],  0x3FACCCCD  ; 1.35
    mov     dword [r12+PJ_MASTER+MS_LP],     0x468CA000  ; 18000
    mov     dword [r12+PJ_MASTER+MS_LPQ],    0x3F666666  ; 0.90
    mov     dword [r12+PJ_MASTER+MS_HP],     0x41E00000  ; 28
    mov     dword [r12+PJ_MASTER+MS_HPQ],    0x3F333333  ; 0.70
    mov     dword [r12+PJ_MASTER+MS_LPON],   1
    mov     dword [r12+PJ_MASTER+MS_HPON],   1
    mov     dword [r12+PJ_MASTER+MS_BPM],    0x42F00000  ; 120
    mov     dword [r12+PJ_MASTER+MS_SWING],  0
    mov     dword [r12+PJ_MASTER+MS_BEATS],  4
    mov     dword [r12+PJ_MASTER+MS_UNIT],   4
    mov     dword [r12+PJ_MASTER+MS_SONGMODE], 0
    ; ---- Song 链 ----
    mov     dword [r12+PJ_SONGCOUNT], 2
    mov     dword [r12+PJ_SONG+0], 0
    mov     dword [r12+PJ_SONG+4], 4
    mov     dword [r12+PJ_SONG+8], 0
    mov     dword [r12+PJ_SONG+12], 4
    EPILOG

; ==========================================================================
;  xu_pattern_clear_steps —— 清空 16×64 格
;  rcx = XuPattern*
; ==========================================================================
global xu_pattern_clear_steps
xu_pattern_clear_steps:
    xor     r8d, r8d
.loop:
    mov     eax, r8d
    shl     eax, 3
    movsxd  rax, eax
    lea     rdx, [rcx+PAT_DATA]
    add     rdx, rax
    mov     dword [rdx], 0
    mov     word [rdx+ST_MICRO], 0
    mov     byte [rdx+ST_RATCHET], 1
    mov     byte [rdx+ST_COND], 0
    mov     byte [rdx+ST_VEL], 100
    mov     byte [rdx+ST_PROB], 100
    inc     r8d
    cmp     r8d, 1024
    jb      .loop
    ret

; ==========================================================================
;  xu_engine_init —— rcx=e, rdx=project, r8=voices
; ==========================================================================
global xu_engine_init
xu_engine_init:
    PROLOG  96
    mov     r12, rcx
    mov     r13, rdx
    mov     r14, r8
    mov     rdi, r12
    mov     rcx, EN_SIZE
    call    xu_bzero
    ; voice 数组也要清（C 版逐个 memset），否则重新初始化会带着上一批正在响的槌
    mov     rdi, r14
    mov     rcx, V_SIZE * 64
    call    xu_bzero
    mov     [r12+EN_PROJ], r13
    mov     [r12+EN_VOICES], r14
    mov     eax, [r13+PJ_SEED]
    test    eax, eax
    jnz     .seed_ok
    mov     eax, 0x808808
.seed_ok:
    mov     [r12+EN_RNG], eax
    mov     dword [r12+EN_PENDINGPAT], -1
    mov     dword [r12+EN_SOLO], -1
    lea     rax, [rel g_evbuf]
    mov     [r12+EN_EVBUF], rax
    ; 64 个 voice 的确定性噪声池
    xor     r15d, r15d
.vloop:
    mov     rax, r15
    imul    rax, rax, V_SIZE
    add     rax, r14
    mov     ebx, r15d
    shr     ebx, 2
    mov     [rax+V_PAD], ebx
    imul    ecx, ebx, 7919
    add     ecx, 0x808
    xor     edx, edx
.nloop:
    imul    ecx, ecx, 1664525
    add     ecx, 1013904223
    mov     r8d, ecx
    shr     r8d, 8
    cvtsi2sd xmm0, r8d
    mulsd   xmm0, [c_inv8388608]
    subsd   xmm0, [c_one]
    cvtsd2ss xmm0, xmm0
    movss   [rax+V_NOISE+rdx*4], xmm0
    inc     edx
    cmp     edx, 8192
    jb      .nloop
    inc     r15d
    cmp     r15d, 64
    jb      .vloop
    EPILOG

; ==========================================================================
;  传输
; ==========================================================================
global xu_engine_play
xu_engine_play:
    cmp     dword [rcx+EN_PLAYING], 0
    jne     .ret
    mov     dword [rcx+EN_PLAYING], 1
    mov     dword [rcx+EN_STEPINDEX], 0
    mov     qword [rcx+EN_GLOBALSTEP], 0
    mov     dword [rcx+EN_CYCLECOUNT], 0
    mov     dword [rcx+EN_FILLNOW], 0
    mov     dword [rcx+EN_SONGINDEX], 0
    mov     dword [rcx+EN_SONGREPEAT], 0
    mov     rax, [rcx+EN_SAMPLEPOS]
    mov     [rcx+EN_NEXTSTEP], rax
    mov     dword [rcx+EN_PENDINGCOUNT], 0
.ret:
    ret

global xu_engine_stop
xu_engine_stop:
    PROLOG  96
    mov     r12, rcx
    mov     dword [r12+EN_PLAYING], 0
    mov     r13, [r12+EN_SAMPLEPOS]
    xor     r14d, r14d
.loop:
    mov     rax, r14
    imul    rax, rax, V_SIZE
    add     rax, [r12+EN_VOICES]
    mov     rcx, rax
    mov     rdx, r13
    mov     r8d, 480
    call    xu_voice_kill
    inc     r14d
    cmp     r14d, 64
    jb      .loop
    mov     dword [r12+EN_PENDINGCOUNT], 0
    EPILOG

; ==========================================================================
;  xu_step_len —— 每步样本数 = 48000*60/BPM/4
; ==========================================================================
global xu_step_len
xu_step_len:
    mov     rax, [rcx+EN_PROJ]
    movss   xmm0, [rax+PJ_MASTER+MS_BPM]
    cvtss2sd xmm0, xmm0
    maxsd   xmm0, [c_40]
    minsd   xmm0, [c_300]
    movsd   xmm1, [c_720000]
    divsd   xmm1, xmm0
    addsd   xmm1, [c_half]
    cvttsd2si eax, xmm1
    cmp     eax, 64
    jge     .ok
    mov     eax, 64
.ok:
    ret

; xorshift32
global xu_next_rand
xu_next_rand:
    mov     eax, [rcx+EN_RNG]
    mov     edx, eax
    shl     edx, 13
    xor     eax, edx
    mov     edx, eax
    shr     edx, 17
    xor     eax, edx
    mov     edx, eax
    shl     edx, 5
    xor     eax, edx
    mov     [rcx+EN_RNG], eax
    ret

; 条件触发：rcx=e, edx=cond → eax
global xu_cond_ok
xu_cond_ok:
    cmp     edx, COND_ALWAYS
    je      .yes
    cmp     edx, COND_FILL
    je      .fill
    cmp     edx, COND_NOTFILL
    je      .notfill
    cmp     edx, COND_EVERY2
    je      .every2
    cmp     edx, COND_EVERY4
    je      .every4
.yes:
    mov     eax, 1
    ret
.fill:
    mov     eax, [rcx+EN_FILLNOW]
    test    eax, eax
    setnz   al
    movzx   eax, al
    ret
.notfill:
    mov     eax, [rcx+EN_FILLNOW]
    test    eax, eax
    setz    al
    movzx   eax, al
    ret
.every2:
    mov     eax, [rcx+EN_CYCLECOUNT]
    test    eax, 1
    setz    al
    movzx   eax, al
    ret
.every4:
    mov     eax, [rcx+EN_CYCLECOUNT]
    and     eax, 3
    setz    al
    movzx   eax, al
    ret

; ==========================================================================
;  xu_push_event —— 追加事件（队列满丢弃最新）
;  rdi=arr, rsi=&count, r9=t, ecx=pad, xmm0=vel, r8d=accent
; ==========================================================================
global xu_push_event
xu_push_event:
    mov     eax, [rsi]
    cmp     eax, XU_MAX_EVENTS
    jb      .space
    ret
.space:
    mov     r10d, eax
    shl     rax, 5
    add     rdi, rax
    mov     [rdi+E_T], r9
    mov     [rdi+E_PAD], ecx
    movsd   [rdi+E_VEL], xmm0
    mov     [rdi+E_ACCENT], r8d
    inc     r10d
    mov     [rsi], r10d
    ret

; ==========================================================================
;  xu_emit_step —— 展开一步的所有触发（rcx=e, rdx=stepAbs）
; ==========================================================================
global xu_emit_step
xu_emit_step:
    PROLOG  256
    mov     r12, rcx
    mov     r13, rdx
    mov     rax, [r12+EN_PROJ]
    mov     [rbp-104], rax
    mov     ecx, [rax+PJ_CURRENT]
    movsxd  rcx, ecx
    imul    rcx, rcx, PAT_SIZE
    add     rcx, rax
    mov     [rbp-88], rcx               ; pat
    mov     rcx, r12
    call    xu_step_len
    mov     [rbp-64], eax               ; sl
    ; swing：奇数步偏移 = sl * swing * 0.5
    xor     eax, eax
    mov     edx, [r12+EN_STEPINDEX]
    test    edx, 1
    jz      .no_swing
    mov     rax, [rbp-104]
    movss   xmm0, [rax+PJ_MASTER+MS_SWING]
    cvtss2sd xmm0, xmm0
    movsxd  rax, dword [rbp-64]
    cvtsi2sd xmm1, rax
    mulsd   xmm1, xmm0
    mulsd   xmm1, [c_half]
    cvttsd2si eax, xmm1
.no_swing:
    movsxd  rax, eax
    add     rax, r13
    mov     [rbp-72], rax               ; base
    mov     dword [rbp-80], 0           ; pad
.pad_loop:
    cmp     dword [rbp-80], 16
    jge     .done
    mov     rax, [rbp-88]
    mov     ecx, [rbp-80]
    movzx   edx, byte [rax+PAT_TRACKLEN+rcx]
    test    edx, edx
    jg      .tl_ok
    mov     edx, 1
.tl_ok:
    cmp     edx, 64
    jle     .tl_ok2
    mov     edx, 64
.tl_ok2:
    mov     rcx, rdx
    mov     rax, [r12+EN_GLOBALSTEP]
    xor     edx, edx
    div     rcx                         ; rdx = globalStep % trackLen
    mov     [rbp-120], edx
    mov     rax, [rbp-88]
    mov     ecx, [rbp-80]
    shl     ecx, 6
    add     ecx, edx
    shl     ecx, 3
    movsxd  rcx, ecx
    add     rax, rcx
    add     rax, PAT_DATA
    mov     [rbp-96], rax               ; XuStep*
    cmp     byte [rax+ST_ON], 0
    je      .next
    movzx   edx, byte [rax+ST_COND]
    mov     rcx, r12
    call    xu_cond_ok
    test    eax, eax
    jz      .next
    mov     rax, [rbp-96]
    movzx   ecx, byte [rax+ST_PROB]
    test    ecx, ecx
    jz      .next
    cmp     ecx, 100
    jge     .prob_ok
    mov     rcx, r12
    call    xu_next_rand
    xor     edx, edx
    mov     ecx, 100
    div     ecx
    mov     rax, [rbp-96]
    movzx   ecx, byte [rax+ST_PROB]
    cmp     edx, ecx
    jge     .next
.prob_ok:
    mov     rax, [rbp-96]
    movzx   eax, byte [rax+ST_VEL]
    cvtsi2sd xmm0, eax
    divsd   xmm0, [c_127]
    maxsd   xmm0, [c_002]
    minsd   xmm0, [c_one]
    mov     rax, [rbp-96]
    cmp     byte [rax+ST_ACC], 0
    je      .no_acc
    mulsd   xmm0, [c_130]
    minsd   xmm0, [c_125]
.no_acc:
    movsd   [rbp-144], xmm0             ; vel
    mov     rax, [rbp-96]
    movzx   eax, byte [rax+ST_RATCHET]
    test    eax, eax
    jg      .r_ok
    mov     eax, 1
.r_ok:
    cmp     eax, 8
    jle     .r_ok2
    mov     eax, 8
.r_ok2:
    mov     [rbp-128], eax
    mov     dword [rbp-136], 0
.i_loop:
    mov     eax, [rbp-136]
    cmp     eax, [rbp-128]
    jge     .next
    ; t = base + micro + floor(i*sl/r)
    mov     rax, [rbp-96]
    movsx   ecx, word [rax+ST_MICRO]
    movsxd  rcx, ecx
    add     rcx, [rbp-72]
    movsxd  r8, dword [rbp-136]
    cvtsi2sd xmm0, r8
    movsxd  r8, dword [rbp-64]
    cvtsi2sd xmm1, r8
    mulsd   xmm0, xmm1
    movsxd  r8, dword [rbp-128]
    cvtsi2sd xmm1, r8
    divsd   xmm0, xmm1
    cvttsd2si rax, xmm0
    add     rcx, rax
    mov     r9, rcx
    ; vv = vel * 0.92^i
    lea     rdx, [rel ratchet_gain]
    movsxd  r8, dword [rbp-136]
    movsd   xmm0, [rdx+r8*8]
    mulsd   xmm0, [rbp-144]
    mov     rax, [rbp-96]
    movzx   r8d, byte [rax+ST_ACC]
    lea     rdi, [r12+EN_PENDING]
    lea     rsi, [r12+EN_PENDINGCOUNT]
    mov     ecx, [rbp-80]
    call    xu_push_event
    inc     dword [rbp-136]
    jmp     .i_loop
.next:
    inc     dword [rbp-80]
    jmp     .pad_loop
.done:
    EPILOG

; ==========================================================================
;  xu_advance_step —— 步进推进 / pattern 切换 / Song 链
;  rcx=e
; ==========================================================================
global xu_advance_step
xu_advance_step:
    PROLOG  256
    mov     r12, rcx
    mov     rax, [r12+EN_PROJ]
    mov     [rbp-64], rax               ; proj
    mov     ecx, [rax+PJ_CURRENT]
    movsxd  rcx, ecx
    imul    rcx, rcx, PAT_SIZE
    add     rcx, rax
    mov     [rbp-72], rcx               ; pat
    inc     dword [r12+EN_STEPINDEX]
    inc     qword [r12+EN_GLOBALSTEP]
    mov     eax, [rcx+PAT_STEPS]
    test    eax, eax
    jg      .steps_ok
    mov     eax, 1
.steps_ok:
    cmp     [r12+EN_STEPINDEX], eax
    jl      .ret
    mov     dword [r12+EN_STEPINDEX], 0
    inc     dword [r12+EN_CYCLECOUNT]
    mov     eax, [r12+EN_CYCLECOUNT]
    and     eax, 3
    cmp     eax, 3
    sete    al
    movzx   eax, al
    mov     [r12+EN_FILLNOW], eax
    ; 待切换 pattern：只在循环边界
    mov     eax, [r12+EN_PENDINGPAT]
    test    eax, eax
    js      .song
    cmp     eax, XU_PATTERNS
    jl      .pat_ok
    mov     eax, XU_PATTERNS - 1
.pat_ok:
    mov     rdx, [rbp-64]
    mov     [rdx+PJ_CURRENT], eax
    mov     dword [r12+EN_PENDINGPAT], -1
    mov     dword [r12+EN_SONGREPEAT], 0
    jmp     .ret
.song:
    mov     rdx, [rbp-64]
    cmp     dword [rdx+PJ_MASTER+MS_SONGMODE], 0
    je      .ret
    mov     r8d, [rdx+PJ_SONGCOUNT]
    test    r8d, r8d
    jle     .ret
    ; en = &song[songIndex % songCount]
    mov     eax, [r12+EN_SONGINDEX]
    xor     edx, edx
    div     r8d                          ; eax = idx, edx = 余数
    mov     ecx, edx
    shl     ecx, 3
    mov     r9, [rbp-64]
    lea     r9, [r9+PJ_SONG+rcx]         ; en
    inc     dword [r12+EN_SONGREPEAT]
    mov     r10d, [r9+4]                 ; repeats
    test    r10d, r10d
    jg      .rep_ok
    mov     r10d, 1
.rep_ok:
    mov     eax, [r12+EN_SONGREPEAT]
    cmp     eax, r10d
    jl      .ret
    mov     dword [r12+EN_SONGREPEAT], 0
    mov     eax, [r12+EN_SONGINDEX]
    inc     eax
    xor     edx, edx
    div     r8d
    mov     [r12+EN_SONGINDEX], edx
    ; np = song[songIndex].pattern
    mov     eax, edx
    shl     eax, 3
    mov     r9, [rbp-64]
    mov     ecx, [r9+PJ_SONG+rax]
    test    ecx, ecx
    jns     .np_ok
    xor     ecx, ecx
.np_ok:
    cmp     ecx, XU_PATTERNS
    jl      .np_ok2
    mov     ecx, XU_PATTERNS - 1
.np_ok2:
    mov     [r9+PJ_CURRENT], ecx
.ret:
    EPILOG

; ==========================================================================
;  en_audible —— rcx=e, edx=pad → eax (1=可闻)
; ==========================================================================
en_audible:
    mov     eax, [rcx+EN_SOLO]
    test    eax, eax
    js      .no_override
    cmp     edx, eax
    sete    al
    movzx   eax, al
    ret
.no_override:
    mov     r8, [rcx+EN_PROJ]
    lea     r8, [r8+PJ_PADS]
    xor     r9d, r9d                    ; anySolo
    xor     r10d, r10d
.sloop:
    cmp     r10d, 16
    jge     .solo_done
    mov     rax, r10
    imul    rax, rax, PP_SIZE
    cmp     dword [r8+rax+PP_SOLO], 0
    je      .snext
    mov     r9d, 1
    jmp     .solo_done
.snext:
    inc     r10d
    jmp     .sloop
.solo_done:
    movsxd  rdx, edx
    imul    rdx, rdx, PP_SIZE
    add     rdx, r8                     ; &pads[pad]
    cmp     dword [rdx+PP_MUTE], 0
    jne     .no
    test    r9d, r9d
    jz      .yes
    cmp     dword [rdx+PP_SOLO], 0
    je      .no
.yes:
    mov     eax, 1
    ret
.no:
    xor     eax, eax
    ret

; ==========================================================================
;  en_render_voices —— rcx=e, rdx=ctx
;  ctx: +0 L, +8 R, +16 b0, +24 a, +32 b, +40 n
; ==========================================================================
en_render_voices:
    PROLOG  256
    mov     r12, rcx
    mov     r13, rdx
    mov     rax, [r13+32]
    cmp     rax, [r13+24]
    jle     .done
    xor     r14d, r14d
.pad_loop:
    cmp     r14d, 16
    jge     .done
    mov     rcx, r12
    mov     edx, r14d
    call    en_audible
    test    eax, eax
    jz      .next
    ; params = &e->proj->pads[pad]
    mov     rax, [r12+EN_PROJ]
    lea     rax, [rax+PJ_PADS]
    imul    rdx, r14, PP_SIZE
    add     rax, rdx
    mov     [rbp-64], rax
    xor     r15d, r15d
.v_loop:
    cmp     r15d, 4
    jge     .next
    mov     rax, r15
    imul    rax, rax, V_SIZE
    mov     rdx, r14
    imul    rdx, rdx, V_SIZE*4
    add     rax, rdx
    add     rax, [r12+EN_VOICES]
    mov     r10, rax
    cmp     dword [r10+V_ACTIVE], 0
    je      .v_next
    mov     rcx, r10
    mov     rdx, [r13+0]
    mov     r8,  [r13+8]
    mov     r9d, [r13+40]
    mov     rax, [r13+16]
    mov     [rsp+32], rax
    mov     rax, [r13+24]
    mov     [rsp+40], rax
    mov     rax, [r13+32]
    mov     [rsp+48], rax
    mov     rax, [rbp-64]
    mov     [rsp+56], rax
    call    xu_voice_render
.v_next:
    inc     r15d
    jmp     .v_loop
.next:
    inc     r14d
    jmp     .pad_loop
.done:
    EPILOG

; ==========================================================================
;  en_trigger —— rcx=e, edx=pad, r8=when, xmm0=vel(double), r9d=accent
; ==========================================================================
en_trigger:
    PROLOG  256
    mov     r12, rcx
    mov     eax, edx
    mov     [rbp-64], rax               ; pad（零扩展后存 64 位）
    mov     [rbp-72], r8                ; when
    mov     [rbp-80], r9d               ; accent
    movsd   [rbp-88], xmm0
    ; 闭镲 / 开镲互掐
    cmp     edx, 14
    je      .choke
    cmp     edx, 15
    jne     .pick
.choke:
    mov     rax, 14
    imul    rax, rax, V_SIZE*4
    add     rax, [r12+EN_VOICES]
    mov     [rbp-96], rax               ; &voices[14][0]
    xor     ebx, ebx
.kloop:
    cmp     ebx, 4
    jge     .pick
    mov     rax, rbx
    imul    rax, rax, V_SIZE
    add     rax, [rbp-96]
    mov     rcx, rax
    mov     rdx, [rbp-72]
    mov     r8d, 16
    call    xu_voice_kill
    inc     ebx
    jmp     .kloop
.pick:
    ; base = &voices[pad][0]
    mov     rax, [rbp-64]
    imul    rax, rax, V_SIZE*4
    add     rax, [r12+EN_VOICES]
    mov     [rbp-96], rax
    xor     ebx, ebx
.find:
    cmp     ebx, 4
    jge     .steal
    mov     rax, rbx
    imul    rax, rax, V_SIZE
    add     rax, [rbp-96]
    cmp     dword [rax+V_ACTIVE], 0
    jne     .fnext
    mov     [rbp-104], rax
    jmp     .have
.fnext:
    inc     ebx
    jmp     .find
.steal:
    ; 抢占最老的 voice
    mov     rax, [rbp-96]
    mov     [rbp-104], rax
    mov     r15, [rax+V_START]
    mov     r14d, 1
.sloop:
    cmp     r14d, 4
    jge     .skilled
    mov     rax, r14
    imul    rax, rax, V_SIZE
    add     rax, [rbp-96]
    cmp     [rax+V_START], r15
    jge     .snext
    mov     [rbp-104], rax
    mov     r15, [rax+V_START]
.snext:
    inc     r14d
    jmp     .sloop
.skilled:
    mov     rcx, [rbp-104]
    mov     rdx, [rbp-72]
    mov     r8d, 64
    call    xu_voice_kill
.have:
    mov     rcx, [rbp-104]
    mov     rdx, [rbp-72]
    movsd   xmm0, [rbp-88]
    mov     r8d, [rbp-80]
    call    xu_voice_trigger
    EPILOG

; ==========================================================================
;  en_collect —— 收集本块事件（rcx=e, rdx=b0, r8=b1, r9=out, [rsp+32]=&count）
;  Win64：第 5 参数在调用方 [rsp+32]，本函数 push rbp 后为 [rbp+48]
; ==========================================================================
en_collect:
    PROLOG  256
    mov     r12, rcx
    mov     r13, rdx
    mov     r14, r8
    mov     r15, r9
    mov     rax, [rbp+48]
    mov     [rbp-64], rax
    mov     dword [rax], 0
    mov     rdi, rax
    ; ---------- 手点事件 ----------
    xor     ebx, ebx
    xor     esi, esi
.mloop:
    mov     eax, [r12+EN_MANUALCOUNT]
    cmp     ebx, eax
    jge     .mdone
    mov     rax, rbx
    shl     rax, 5
    lea     rdx, [r12+EN_MANUAL]
    add     rdx, rax
    mov     rax, [rdx+E_T]
    cmp     rax, r14
    jl      .mtake
    mov     rax, rsi
    shl     rax, 5
    lea     r8, [r12+EN_MANUAL]
    add     r8, rax
    mov     rax, [rdx]
    mov     [r8], rax
    mov     rax, [rdx+8]
    mov     [r8+8], rax
    mov     rax, [rdx+16]
    mov     [r8+16], rax
    mov     rax, [rdx+24]
    mov     [r8+24], rax
    inc     esi
    jmp     .mnext
.mtake:
    cmp     rax, r13
    jge     .mclamp
    mov     rax, r13
    mov     [rdx+E_T], rax
.mclamp:
    mov     ecx, [rdi]
    mov     r8d, ecx
    shl     r8, 5
    add     r8, r15
    mov     rax, [rdx]
    mov     [r8], rax
    mov     rax, [rdx+8]
    mov     [r8+8], rax
    mov     rax, [rdx+16]
    mov     [r8+16], rax
    mov     rax, [rdx+24]
    mov     [r8+24], rax
    inc     ecx
    mov     [rdi], ecx
.mnext:
    inc     ebx
    jmp     .mloop
.mdone:
    mov     [r12+EN_MANUALCOUNT], esi
    ; ---------- 音序器推进 ----------
    cmp     dword [r12+EN_PLAYING], 0
    je      .pend
    cmp     dword [r12+EN_CLOCKEXT], 0
    jne     .pend
    xor     ebx, ebx
.sq_loop:
    mov     rax, [r12+EN_NEXTSTEP]
    cmp     rax, r14
    jge     .pend
    cmp     ebx, 64
    jge     .pend
    mov     rdx, rax
    cmp     rdx, r13
    jge     .sq_ok
    mov     rdx, r13
.sq_ok:
    mov     rcx, r12
    call    xu_emit_step
    mov     rcx, r12
    call    xu_step_len
    movsxd  rax, eax
    add     [r12+EN_NEXTSTEP], rax
    mov     rcx, r12
    call    xu_advance_step
    inc     ebx
    jmp     .sq_loop
    ; ---------- 音序器事件 ----------
.pend:
    xor     ebx, ebx
    xor     esi, esi
.ploop:
    mov     eax, [r12+EN_PENDINGCOUNT]
    cmp     ebx, eax
    jge     .pdone
    mov     rax, rbx
    shl     rax, 5
    lea     rdx, [r12+EN_PENDING]
    add     rdx, rax
    mov     rax, [rdx+E_T]
    cmp     rax, r14
    jl      .ptake
    mov     rax, rsi
    shl     rax, 5
    lea     r8, [r12+EN_PENDING]
    add     r8, rax
    mov     rax, [rdx]
    mov     [r8], rax
    mov     rax, [rdx+8]
    mov     [r8+8], rax
    mov     rax, [rdx+16]
    mov     [r8+16], rax
    mov     rax, [rdx+24]
    mov     [r8+24], rax
    inc     esi
    jmp     .pnext
.ptake:
    cmp     rax, r13
    jge     .pclamp
    mov     rax, r13
    mov     [rdx+E_T], rax
.pclamp:
    mov     ecx, [rdi]
    mov     r8d, ecx
    shl     r8, 5
    add     r8, r15
    mov     rax, [rdx]
    mov     [r8], rax
    mov     rax, [rdx+8]
    mov     [r8+8], rax
    mov     rax, [rdx+16]
    mov     [r8+16], rax
    mov     rax, [rdx+24]
    mov     [r8+24], rax
    inc     ecx
    mov     [rdi], ecx
.pnext:
    inc     ebx
    jmp     .ploop
.pdone:
    mov     [r12+EN_PENDINGCOUNT], esi
    ; ---------- 插入排序 ----------
    mov     r10d, 1
.sort_outer:
    cmp     r10d, [rdi]
    jge     .sort_done
    mov     rax, r10
    shl     rax, 5
    add     rax, r15
    mov     rcx, [rax]
    mov     [rbp-128], rcx
    mov     rcx, [rax+8]
    mov     [rbp-120], rcx
    mov     rcx, [rax+16]
    mov     [rbp-112], rcx
    mov     rcx, [rax+24]
    mov     [rbp-104], rcx
    mov     r11d, r10d
    dec     r11d
.sort_inner:
    cmp     r11d, 0
    jl      .sort_place
    movsxd  rdx, r11d                   ; 注意：dec r11d 会清高 32 位，不能直接当 64 位用
    shl     rdx, 5
    add     rdx, r15
    mov     rcx, [rdx]
    cmp     rcx, [rbp-128]
    jle     .sort_place
    lea     r8, [rdx+32]
    mov     rcx, [rdx]
    mov     [r8], rcx
    mov     rcx, [rdx+8]
    mov     [r8+8], rcx
    mov     rcx, [rdx+16]
    mov     [r8+16], rcx
    mov     rcx, [rdx+24]
    mov     [r8+24], rcx
    dec     r11d
    jmp     .sort_inner
.sort_place:
    movsxd  rax, r11d
    inc     rax
    shl     rax, 5
    add     rax, r15
    mov     rcx, [rbp-128]
    mov     [rax], rcx
    mov     rcx, [rbp-120]
    mov     [rax+8], rcx
    mov     rcx, [rbp-112]
    mov     [rax+16], rcx
    mov     rcx, [rbp-104]
    mov     [rax+24], rcx
    inc     r10d
    jmp     .sort_outer
.sort_done:
    EPILOG

; ==========================================================================
;  xu_engine_render —— rcx=e, edx=n, r8=L, r9=R
; ==========================================================================
global xu_engine_render
xu_engine_render:
    PROLOG  320
    mov     r12, rcx
    mov     eax, edx
    mov     [rbp-72], rax               ; n
    mov     [rbp-80], r8                ; L
    mov     [rbp-88], r9                ; R
    mov     rax, [r12+EN_SAMPLEPOS]
    mov     [rbp-96], rax               ; b0
    mov     rcx, [rbp-72]
    add     rcx, rax
    mov     [rbp-104], rcx              ; b1
    ; 清零 L/R
    mov     rdi, [rbp-80]
    mov     ecx, [rbp-72]
    xor     eax, eax
    rep     stosd
    mov     rdi, [rbp-88]
    mov     ecx, [rbp-72]
    xor     eax, eax
    rep     stosd
    ; ---------- 收集事件 ----------
    mov     dword [rbp-112], 0
    mov     rcx, r12
    mov     rdx, [rbp-96]
    mov     r8, [rbp-104]
    lea     r9, [rel g_evbuf]
    lea     rax, [rbp-112]
    mov     [rsp+32], rax
    call    en_collect
    ; ---------- 按事件切段渲染 ----------
    mov     rax, [rbp-96]
    mov     [rbp-120], rax              ; cur
    mov     r14, [rbp-80]
    mov     r15, [rbp-88]
    xor     ebx, ebx
.ev_loop:
    cmp     ebx, [rbp-112]
    jge     .ev_done
    mov     rax, rbx
    shl     rax, 5
    lea     r13, [rel g_evbuf]
    add     r13, rax
    mov     rax, [r13+E_T]
    cmp     rax, [rbp-120]
    jle     .ev_trig
    ; 渲染 [cur, ev.t)
    lea     rdx, [rbp-176]
    mov     [rdx+0], r14
    mov     [rdx+8], r15
    mov     rax, [rbp-96]
    mov     [rdx+16], rax
    mov     rax, [rbp-120]
    mov     [rdx+24], rax
    mov     rax, [r13+E_T]
    mov     [rdx+32], rax
    mov     eax, [rbp-72]
    mov     [rdx+40], eax
    mov     rcx, r12
    call    en_render_voices
    mov     rax, [r13+E_T]
    mov     [rbp-120], rax
.ev_trig:
    mov     rax, [r13+E_T]
    cmp     rax, [rbp-104]
    jge     .ev_next
    mov     rcx, r12
    mov     edx, [r13+E_PAD]
    mov     r8, rax
    movsd   xmm0, [r13+E_VEL]
    mov     r9d, [r13+E_ACCENT]
    call    en_trigger
.ev_next:
    inc     ebx
    jmp     .ev_loop
.ev_done:
    mov     rax, [rbp-120]
    cmp     rax, [rbp-104]
    jge     .fx
    lea     rdx, [rbp-176]
    mov     [rdx+0], r14
    mov     [rdx+8], r15
    mov     rax, [rbp-96]
    mov     [rdx+16], rax
    mov     rax, [rbp-120]
    mov     [rdx+24], rax
    mov     rax, [rbp-104]
    mov     [rdx+32], rax
    mov     eax, [rbp-72]
    mov     [rdx+40], eax
    mov     rcx, r12
    call    en_render_voices
.fx:
    ; ---------- 主效果链 ----------
    mov     r13, [r12+EN_PROJ]
    add     r13, PJ_MASTER
    mov     r14, [rbp-80]
    mov     r15, [rbp-88]
    cmp     dword [r13+MS_HPON], 0
    je      .no_hp
    movss   xmm0, [r13+MS_HP]
    cvtss2sd xmm0, xmm0
    movss   xmm1, [r13+MS_HPQ]
    cvtss2sd xmm1, xmm1
    lea     rcx, [r12+EN_HPL]
    call    xu_bq_hp
    movss   xmm0, [r13+MS_HP]
    cvtss2sd xmm0, xmm0
    movss   xmm1, [r13+MS_HPQ]
    cvtss2sd xmm1, xmm1
    lea     rcx, [r12+EN_HPR]
    call    xu_bq_hp
    xor     ebx, ebx
.hp_loop:
    cmp     rbx, [rbp-72]
    jge     .no_hp
    movss   xmm0, [r14+rbx*4]
    cvtss2sd xmm0, xmm0
    lea     rcx, [r12+EN_HPL]
    call    xu_bq_run
    cvtsd2ss xmm0, xmm0
    movss   [r14+rbx*4], xmm0
    movss   xmm0, [r15+rbx*4]
    cvtss2sd xmm0, xmm0
    lea     rcx, [r12+EN_HPR]
    call    xu_bq_run
    cvtsd2ss xmm0, xmm0
    movss   [r15+rbx*4], xmm0
    inc     rbx
    jmp     .hp_loop
.no_hp:
    cmp     dword [r13+MS_LPON], 0
    je      .no_lp
    movss   xmm0, [r13+MS_LP]
    cvtss2sd xmm0, xmm0
    movss   xmm1, [r13+MS_LPQ]
    cvtss2sd xmm1, xmm1
    lea     rcx, [r12+EN_LPL]
    call    xu_bq_lp
    movss   xmm0, [r13+MS_LP]
    cvtss2sd xmm0, xmm0
    movss   xmm1, [r13+MS_LPQ]
    cvtss2sd xmm1, xmm1
    lea     rcx, [r12+EN_LPR]
    call    xu_bq_lp
    xor     ebx, ebx
.lp_loop:
    cmp     rbx, [rbp-72]
    jge     .no_lp
    movss   xmm0, [r14+rbx*4]
    cvtss2sd xmm0, xmm0
    lea     rcx, [r12+EN_LPL]
    call    xu_bq_run
    cvtsd2ss xmm0, xmm0
    movss   [r14+rbx*4], xmm0
    movss   xmm0, [r15+rbx*4]
    cvtss2sd xmm0, xmm0
    lea     rcx, [r12+EN_LPR]
    call    xu_bq_run
    cvtsd2ss xmm0, xmm0
    movss   [r15+rbx*4], xmm0
    inc     rbx
    jmp     .lp_loop
.no_lp:
    ; drive / 软限幅 / 主音量
    movss   xmm0, [r13+MS_DRIVE]
    maxss   xmm0, [rel ff_one]
    minss   xmm0, [rel ff_eight]
    movss   [rbp-184], xmm0             ; drive
    cvtss2sd xmm0, xmm0
    call    xu_dtanh
    movsd   [rbp-192], xmm0             ; norm
    movss   xmm0, [r13+MS_VOLUME]
    maxss   xmm0, [rel ff_zero]
    minss   xmm0, [rel ff_one]
    movss   [rbp-180], xmm0             ; vol
    xorps   xmm6, xmm6
    xorps   xmm7, xmm7
    xor     ebx, ebx
.fx_loop:
    cmp     rbx, [rbp-72]
    jge     .fx_done
    movss   xmm4, [r14+rbx*4]
    movss   xmm5, [r15+rbx*4]
    movss   xmm0, [rbp-184]
    comiss  xmm0, [rel ff_1_001]
    jbe     .no_drive
    movss   xmm0, xmm4
    mulss   xmm0, [rbp-184]
    cvtss2sd xmm0, xmm0
    call    xu_dtanh
    divsd   xmm0, [rbp-192]
    cvtsd2ss xmm4, xmm0
    movss   xmm0, xmm5
    mulss   xmm0, [rbp-184]
    cvtss2sd xmm0, xmm0
    call    xu_dtanh
    divsd   xmm0, [rbp-192]
    cvtsd2ss xmm5, xmm0
.no_drive:
    movaps  xmm0, xmm4
    andps   xmm0, [rel ff_abs_ps]
    addss   xmm0, [rel ff_one]
    divss   xmm4, xmm0
    movaps  xmm0, xmm5
    andps   xmm0, [rel ff_abs_ps]
    addss   xmm0, [rel ff_one]
    divss   xmm5, xmm0
    mulss   xmm4, [rbp-180]
    mulss   xmm5, [rbp-180]
    movss   [r14+rbx*4], xmm4
    movss   [r15+rbx*4], xmm5
    movaps  xmm0, xmm4
    andps   xmm0, [rel ff_abs_ps]
    maxss   xmm6, xmm0
    movaps  xmm0, xmm5
    andps   xmm0, [rel ff_abs_ps]
    maxss   xmm7, xmm0
    inc     rbx
    jmp     .fx_loop
.fx_done:
    movss   xmm0, [r12+EN_METERL]
    mulss   xmm0, [rel ff_0_75]
    maxss   xmm6, xmm0
    movss   [r12+EN_METERL], xmm6
    movss   xmm0, [r12+EN_METERR]
    mulss   xmm0, [rel ff_0_75]
    maxss   xmm7, xmm0
    movss   [r12+EN_METERR], xmm7
    ; ---------- 时钟推进 / 过期 voice ----------
    mov     rax, [rbp-104]
    mov     [r12+EN_SAMPLEPOS], rax
    xor     ebx, ebx
.dv:
    cmp     ebx, 64
    jge     .dv_done
    mov     rax, rbx
    imul    rax, rax, V_SIZE
    add     rax, [r12+EN_VOICES]
    cmp     dword [rax+V_ACTIVE], 0
    je      .dv_next
    mov     rdx, [rbp-96]
    sub     rdx, [rax+V_START]
    cmp     rdx, 192000
    jle     .dv_next
    mov     dword [rax+V_ACTIVE], 0
.dv_next:
    inc     ebx
    jmp     .dv
.dv_done:
    EPILOG

; ==========================================================================
;  UI 调用
; ==========================================================================
global xu_engine_trigger_pad
; rcx=e, edx=pad, r8d=vel, r9d=accent
xu_engine_trigger_pad:
    PROLOG  96
    test    edx, edx
    js      .ret
    cmp     edx, XU_PADS
    jge     .ret
    mov     r12, rcx
    mov     r13d, edx
    mov     r14d, r8d
    mov     r15d, r9d
    test    r14d, r14d
    jg      .vel_ok
    mov     r14d, 1
.vel_ok:
    cmp     r14d, 127
    jle .vel_ok2
    mov     r14d, 127
.vel_ok2:
    movsxd  rax, r14d
    cvtsi2sd xmm0, rax
    divsd   xmm0, [c_127]
    lea     rdi, [r12+EN_MANUAL]
    lea     rsi, [r12+EN_MANUALCOUNT]
    mov     r9, [r12+EN_SAMPLEPOS]
    add     r9, 32
    mov     ecx, r13d
    mov     r8d, r15d
    call    xu_push_event
.ret:
    EPILOG

global xu_engine_request_pattern
; rcx=e, edx=idx
xu_engine_request_pattern:
    test    edx, edx
    jns     .ok1
    xor     edx, edx
.ok1:
    cmp     edx, XU_PATTERNS
    jl      .ok2
    mov     edx, XU_PATTERNS - 1
.ok2:
    cmp     dword [rcx+EN_PLAYING], 0
    je      .direct
    mov     [rcx+EN_PENDINGPAT], edx
    ret
.direct:
    mov     rax, [rcx+EN_PROJ]
    mov     [rax+PJ_CURRENT], edx
    ret

global xu_engine_external_tick
xu_engine_external_tick:
    PROLOG  96
    mov     r12, rcx
    cmp     dword [r12+EN_PLAYING], 0
    je      .ret
    inc     dword [r12+EN_CLOCKPULSE]
    cmp     dword [r12+EN_CLOCKPULSE], 6
    jl      .ret
    mov     dword [r12+EN_CLOCKPULSE], 0
    mov     rcx, r12
    mov     rdx, [r12+EN_SAMPLEPOS]
    call    xu_emit_step
    mov     rcx, r12
    call    xu_advance_step
    mov     rcx, r12
    call    xu_step_len
    movsxd  rax, eax
    add     rax, [r12+EN_SAMPLEPOS]
    mov     [r12+EN_NEXTSTEP], rax
.ret:
    EPILOG

; ==========================================================================
;  浮点 → int16 打包
;  rcx=dst, rdx=L, r8=R, r9d=n
; ==========================================================================
xu_pack16:
    xor     r10d, r10d
.loop:
    cmp     r10d, r9d
    jge     .done
    movss   xmm0, [rdx+r10*4]
    movss   xmm1, [r8+r10*4]
    maxss   xmm0, [rel ff_neg1]
    minss   xmm0, [rel ff_pos1]
    maxss   xmm1, [rel ff_neg1]
    minss   xmm1, [rel ff_pos1]
    mulss   xmm0, [rel ff_32767]
    mulss   xmm1, [rel ff_32767]
    cvttss2si eax, xmm0
    cvttss2si r11d, xmm1
    mov     word [rcx+r10*4], ax
    mov     word [rcx+r10*4+2], r11w
    inc     r10d
    jmp     .loop
.done:
    ret

; ==========================================================================
;  waveOut 四缓冲输出
; ==========================================================================
global xu_audio_start
; rcx = engine → eax (1=成功)
xu_audio_start:
    PROLOG  128
    mov     r12, rcx
    cmp     dword [rel g_audio+AU_RUNNING], 0
    jne     .ok
    lea     rdi, [rel g_audio]
    mov     ecx, AU_SIZE
    call    xu_bzero
    mov     [rel g_audio+AU_ENG], r12
    ; WAVEFORMATEX
    lea     rdi, [rel g_audio+AU_FMT]
    mov     word [rdi+0], 1
    mov     word [rdi+2], 2
    mov     dword [rdi+4], XU_SR
    mov     dword [rdi+8], XU_SR*4
    mov     word [rdi+12], 4
    mov     word [rdi+14], 16
    mov     word [rdi+16], 0
    ; waveOutOpen
    lea     rcx, [rel g_audio+AU_HWO]
    mov     edx, -1
    lea     r8, [rel g_audio+AU_FMT]
    lea     r9, [rel xu_wave_cb]
    mov     qword [rsp+32], 0
    mov     qword [rsp+40], 0x30000
    call    waveOutOpen
    test    eax, eax
    jnz     .fail
    xor     ebx, ebx
.hloop:
    cmp     ebx, 4
    jge     .started
    ; hdr
    mov     rax, rbx
    imul    rax, rax, WH_SIZE
    lea     rdx, [rel g_audio+AU_HDR]
    add     rdx, rax
    mov     [rbp-64], rdx
    ; data
    mov     rax, rbx
    imul    rax, rax, XU_BLOCK*4
    lea     rdi, [rel g_audio+AU_DATA]
    add     rdi, rax
    mov     [rbp-72], rdi
    mov     [rdx+WH_LPDATA], rdi
    mov     dword [rdx+WH_LEN], XU_BLOCK*4
    mov     rcx, [rel g_audio+AU_HWO]
    mov     r8d, WH_SIZE
    call    waveOutPrepareHeader
    ; 渲染并预填一块
    mov     rcx, r12
    mov     edx, XU_BLOCK
    lea     r8, [rel g_L]
    lea     r9, [rel g_R]
    call    xu_engine_render
    mov     rcx, [rbp-72]
    lea     rdx, [rel g_L]
    lea     r8, [rel g_R]
    mov     r9d, XU_BLOCK
    call    xu_pack16
    mov     rcx, [rel g_audio+AU_HWO]
    mov     rdx, [rbp-64]
    mov     r8d, WH_SIZE
    call    waveOutWrite
    inc     ebx
    jmp     .hloop
.started:
    mov     dword [rel g_audio+AU_NEXT], 0
    mov     dword [rel g_audio+AU_RUNNING], 1
.ok:
    mov     eax, 1
    EPILOG
.fail:
    xor     eax, eax
    EPILOG

global xu_audio_stop
xu_audio_stop:
    cmp     dword [rel g_audio+AU_RUNNING], 0
    je      .ret
    PROLOG  96
    mov     dword [rel g_audio+AU_RUNNING], 0
    ; 给正在回调里执行 waveOutWrite 的线程一点时间退出，
    ; 否则 waveOutReset 与回调抢设备锁，可能互相等待。
    mov     ecx, 30
    call    Sleep
    mov     rcx, [rel g_audio+AU_HWO]
    call    waveOutReset
    xor     ebx, ebx
.uloop:
    cmp     ebx, 4
    jge     .close
    mov     rax, rbx
    imul    rax, rax, WH_SIZE
    lea     rdx, [rel g_audio+AU_HDR]
    add     rdx, rax
    mov     rcx, [rel g_audio+AU_HWO]
    mov     r8d, WH_SIZE
    call    waveOutUnprepareHeader
    inc     ebx
    jmp     .uloop
.close:
    mov     rcx, [rel g_audio+AU_HWO]
    call    waveOutClose
    mov     qword [rel g_audio+AU_HWO], 0
    EPILOG
.ret:
    ret

global xu_audio_running
xu_audio_running:
    mov     eax, [rel g_audio+AU_RUNNING]
    ret

; --------------------------------------------------------------------------
;  waveOut 回调：WOM_DONE 时渲染下一块并回写
; --------------------------------------------------------------------------
xu_wave_cb:
    cmp     edx, 0x3BD                  ; WOM_DONE
    jne     .ret
    cmp     dword [rel g_audio+AU_RUNNING], 0
    je      .ret
    PROLOG  128
    mov     rcx, [rel g_audio+AU_ENG]
    mov     edx, XU_BLOCK
    lea     r8, [rel g_L]
    lea     r9, [rel g_R]
    call    xu_engine_render
    movsxd  rbx, dword [rel g_audio+AU_NEXT]
    mov     rax, rbx
    imul    rax, rax, XU_BLOCK*4
    lea     rdi, [rel g_audio+AU_DATA]
    add     rdi, rax
    mov     rcx, rdi
    lea     rdx, [rel g_L]
    lea     r8, [rel g_R]
    mov     r9d, XU_BLOCK
    call    xu_pack16
    mov     rax, rbx
    imul    rax, rax, WH_SIZE
    lea     rdx, [rel g_audio+AU_HDR]
    add     rdx, rax
    mov     [rbp-64], rdx
    lea     eax, [rbx+1]
    and     eax, 3
    mov     [rel g_audio+AU_NEXT], eax
    mov     rcx, [rel g_audio+AU_HWO]
    mov     rdx, [rbp-64]
    mov     r8d, WH_SIZE
    call    waveOutWrite
    EPILOG
.ret:
    ret

; ==========================================================================
;  WAV 导出
; ==========================================================================
; xu_total_steps：rcx = project, edx = song → rax
xu_total_steps:
    test    edx, edx
    jz      .single
    mov     eax, [rcx+PJ_SONGCOUNT]
    test    eax, eax
    jle     .single
    xor     r8d, r8d
    xor     r9, r9
.sl:
    cmp     r8d, eax
    jge     .sret
    mov     r10d, [rcx+r8*8+PJ_SONG]
    test    r10d, r10d
    jns     .c1
    xor     r10d, r10d
.c1:
    cmp     r10d, XU_PATTERNS
    jl      .c2
    mov     r10d, XU_PATTERNS - 1
.c2:
    mov     r11d, [rcx+r8*8+PJ_SONG+4]
    test    r11d, r11d
    jg      .c3
    mov     r11d, 1
.c3:
    mov     r10, r10
    imul    r10, r10, PAT_SIZE
    add     r10, rcx
    mov     edx, [r10+PAT_STEPS]
    test    edx, edx
    jg      .c4
    mov     edx, 1
.c4:
    imul    rdx, r11
    add     r9, rdx
    inc     r8d
    mov     eax, [rcx+PJ_SONGCOUNT]
    jmp     .sl
.sret:
    mov     rax, r9
    test    rax, rax
    jg      .r
    mov     eax, 16
.r:
    ret
.single:
    mov     eax, [rcx+PJ_CURRENT]
    test    eax, eax
    jns     .s1
    xor     eax, eax
.s1:
    cmp     eax, XU_PATTERNS
    jl      .s2
    mov     eax, XU_PATTERNS - 1
.s2:
    movsxd  rax, eax
    imul    rax, rax, PAT_SIZE
    add     rax, rcx
    mov     eax, [rax+PAT_STEPS]
    test    eax, eax
    jg      .s3
    mov     eax, 1
.s3:
    ret

global xu_export_wav
; rcx = project, rdx = wchar path, r8d = song, r9d = soloPad → eax
xu_export_wav:
    PROLOG  320
    mov     r12, rcx
    mov     [rbp-64], rdx
    mov     [rbp-72], r8d
    mov     [rbp-80], r9d
    ; 初始化离线引擎
    lea     rcx, [rel g_exp_engine]
    mov     rdx, r12
    lea     r8, [rel g_exp_voices]
    call    xu_engine_init
    mov     eax, [rbp-80]
    mov     [rel g_exp_engine+EN_SOLO], eax
    lea     rcx, [rel g_exp_engine]
    call    xu_engine_play
    ; 步数 / 步长 / 帧数
    mov     rcx, r12
    mov     edx, [rbp-72]
    call    xu_total_steps
    mov     [rbp-88], rax
    lea     rcx, [rel g_exp_engine]
    call    xu_step_len
    movsxd  rax, eax
    mov     [rbp-96], rax
    mov     rax, [rbp-88]
    imul    rax, [rbp-96]
    add     rax, 144000                 ; 末尾 3 秒尾音
    mov     [rbp-104], rax
    shl     rax, 2
    mov     [rbp-112], rax              ; dataBytes
    ; RIFF 头
    lea     rdi, [rbp-192]
    mov     dword [rdi], 'RIFF'
    mov     eax, [rbp-112]
    add     eax, 36
    mov     [rdi+4], eax
    mov     dword [rdi+8], 'WAVE'
    mov     dword [rdi+12], 'fmt '
    mov     dword [rdi+16], 16
    mov     word [rdi+20], 1
    mov     word [rdi+22], 2
    mov     dword [rdi+24], XU_SR
    mov     dword [rdi+28], XU_SR*4
    mov     word [rdi+32], 4
    mov     word [rdi+34], 16
    mov     dword [rdi+36], 'data'
    mov     eax, [rbp-112]
    mov     [rdi+40], eax
    ; 建文件
    mov     rcx, [rbp-64]
    mov     edx, 0x40000000
    xor     r8d, r8d
    xor     r9d, r9d
    mov     qword [rsp+32], 2
    mov     qword [rsp+40], 0x80
    mov     qword [rsp+48], 0
    call    CreateFileW
    cmp     rax, -1
    je      .fail
    mov     [rbp-120], rax
    ; 写头
    mov     rcx, rax
    lea     rdx, [rbp-192]
    mov     r8d, 44
    lea     r9, [rel g_written]
    mov     qword [rsp+32], 0
    call    WriteFile
    test    eax, eax
    jz      .close_fail
    xor     r13, r13
.blk:
    mov     rax, [rbp-104]
    sub     rax, r13
    jle     .done
    mov     r14d, XU_BLOCK
    cmp     rax, XU_BLOCK
    jge     .n_ok
    mov     r14d, eax
.n_ok:
    lea     rcx, [rel g_exp_engine]
    mov     edx, r14d
    lea     r8, [rel g_expL]
    lea     r9, [rel g_expR]
    call    xu_engine_render
    lea     rcx, [rel g_out16]
    lea     rdx, [rel g_expL]
    lea     r8, [rel g_expR]
    mov     r9d, r14d
    call    xu_pack16
    mov     rcx, [rbp-120]
    lea     rdx, [rel g_out16]
    mov     r8d, r14d
    shl     r8d, 2
    lea     r9, [rel g_written]
    mov     qword [rsp+32], 0
    call    WriteFile
    test    eax, eax
    jz      .close_fail
    movsxd  rax, r14d
    add     r13, rax
    jmp     .blk
.done:
    mov     rcx, [rbp-120]
    call    CloseHandle
    mov     eax, 1
    EPILOG
.close_fail:
    mov     rcx, [rbp-120]
    call    CloseHandle
.fail:
    xor     eax, eax
    EPILOG
