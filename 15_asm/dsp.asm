; ==========================================================================
;  dsp.asm —— 十六声部鼓合成 + RBJ 双二阶滤波（x86-64 NASM，SSE2 标量）
;
;  署名：徐某。
;  公式逐项对拍 03_c_win32/dsp.c；区别只有三点：
;    1) 三角函数/指数/tanh 用 mathx.asm 的快速双精度实现；
;    2) 每样本仍按“年龄 t”解析求值，voice 到点自然归零，不做采样循环；
;    3) 噪声池在引擎初始化时一次性填好，音频线程里不做任何分配。
;
;  voice 渲染是唯一会写 L/R 的地方；所有增益、声像、淡出都在这里完成。
; ==========================================================================
%include "xu808.inc"

default rel


%define L_T     -64
%define L_T0    -72
%define L_T1    -80
%define L_T2    -88
%define L_T3    -96
%define L_T4    -104
%define L_T5    -112
%define L_T6    -120
%define L_T7    -128
%define L_T8    -136
%define L_T9    -144

section .text
extern xu_dsin
extern xu_dcos
extern xu_dexp
extern xu_dtanh
extern xu_square

; ==========================================================================
;  RBJ 双二阶系数
;  rcx = XuBiquad*，xmm0 = fc，xmm1 = q
;  参数几乎没变时直接返回，避免每样本重复算 sin/cos。
; ==========================================================================
global xu_bq_lp
xu_bq_lp:
    push    rbx                         ; 对齐栈（本函数内部会 call）
    mov     r10, rcx
    maxsd   xmm0, [c_20]
    minsd   xmm0, [c_23520]
    maxsd   xmm1, [c_01]
    minsd   xmm1, [c_20]
    movapd  xmm15, xmm1                 ; 保存 clamp 后的 q
    cmp     dword [r10+BQ_KIND], 1
    jne     .calc
    movsd   xmm2, [r10+BQ_FC]
    subsd   xmm2, xmm0
    comisd  xmm2, [c_half]
    ja      .calc
    comisd  xmm2, [c_neg_half]
    jb      .calc
    movsd   xmm2, [r10+BQ_Q]
    subsd   xmm2, xmm15
    comisd  xmm2, [c_001]
    ja      .calc
    comisd  xmm2, [c_neg001]
    jb      .calc
    pop     rbx
    ret
.calc:
    movsd   xmm8, xmm0                  ; fc
    mulsd   xmm0, [c_two_pi_over_sr]
    movapd  xmm14, xmm0                 ; w0
    call    xu_dcos
    movapd  xmm11, xmm0                 ; cw
    movapd  xmm0, xmm14
    call    xu_dsin
    movapd  xmm12, xmm0                 ; sw
    movapd  xmm9, xmm15
    mulsd   xmm9, [c_two]
    movapd  xmm13, xmm12
    divsd   xmm13, xmm9                 ; alpha
    movsd   xmm10, [c_one]
    addsd   xmm10, xmm13                ; a0
    movsd   xmm0, [c_one]
    subsd   xmm0, xmm11
    mulsd   xmm0, [c_half]
    divsd   xmm0, xmm10
    movsd   [r10+BQ_B0], xmm0
    movsd   [r10+BQ_B2], xmm0
    movsd   xmm0, [c_one]
    subsd   xmm0, xmm11
    divsd   xmm0, xmm10
    movsd   [r10+BQ_B1], xmm0
    movapd  xmm0, xmm11
    mulsd   xmm0, [c_neg_two]
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A1], xmm0
    movsd   xmm0, [c_one]
    subsd   xmm0, xmm13
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A2], xmm0
    mov     dword [r10+BQ_KIND], 1
    movsd   [r10+BQ_FC], xmm8
    movsd   [r10+BQ_Q], xmm15
    pop     rbx
    ret

global xu_bq_hp
xu_bq_hp:
    push    rbx
    mov     r10, rcx
    maxsd   xmm0, [c_10]
    minsd   xmm0, [c_23520]
    maxsd   xmm1, [c_01]
    minsd   xmm1, [c_20]
    movapd  xmm15, xmm1
    cmp     dword [r10+BQ_KIND], 2
    jne     .calc
    movsd   xmm2, [r10+BQ_FC]
    subsd   xmm2, xmm0
    comisd  xmm2, [c_half]
    ja      .calc
    comisd  xmm2, [c_neg_half]
    jb      .calc
    movsd   xmm2, [r10+BQ_Q]
    subsd   xmm2, xmm15
    comisd  xmm2, [c_001]
    ja      .calc
    comisd  xmm2, [c_neg001]
    jb      .calc
    pop     rbx
    ret
.calc:
    movsd   xmm8, xmm0
    mulsd   xmm0, [c_two_pi_over_sr]
    movapd  xmm14, xmm0
    call    xu_dcos
    movapd  xmm11, xmm0
    movapd  xmm0, xmm14
    call    xu_dsin
    movapd  xmm12, xmm0
    movapd  xmm9, xmm15
    mulsd   xmm9, [c_two]
    movapd  xmm13, xmm12
    divsd   xmm13, xmm9
    movsd   xmm10, [c_one]
    addsd   xmm10, xmm13
    movsd   xmm0, [c_one]
    addsd   xmm0, xmm11
    mulsd   xmm0, [c_half]
    divsd   xmm0, xmm10
    movsd   [r10+BQ_B0], xmm0
    movsd   [r10+BQ_B2], xmm0
    movsd   xmm0, [c_one]
    addsd   xmm0, xmm11
    xorps   xmm3, xmm3
    subsd   xmm3, xmm0                  ; -(1+cw)
    movapd  xmm0, xmm3
    divsd   xmm0, xmm10
    movsd   [r10+BQ_B1], xmm0
    movapd  xmm0, xmm11
    mulsd   xmm0, [c_neg_two]
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A1], xmm0
    movsd   xmm0, [c_one]
    subsd   xmm0, xmm13
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A2], xmm0
    mov     dword [r10+BQ_KIND], 2
    movsd   [r10+BQ_FC], xmm8
    movsd   [r10+BQ_Q], xmm15
    pop     rbx
    ret

global xu_bq_bp
xu_bq_bp:
    push    rbx
    mov     r10, rcx
    maxsd   xmm0, [c_20]
    minsd   xmm0, [c_23520]
    maxsd   xmm1, [c_01]
    minsd   xmm1, [c_30]
    movapd  xmm15, xmm1
    cmp     dword [r10+BQ_KIND], 3
    jne     .calc
    movsd   xmm2, [r10+BQ_FC]
    subsd   xmm2, xmm0
    comisd  xmm2, [c_half]
    ja      .calc
    comisd  xmm2, [c_neg_half]
    jb      .calc
    movsd   xmm2, [r10+BQ_Q]
    subsd   xmm2, xmm15
    comisd  xmm2, [c_001]
    ja      .calc
    comisd  xmm2, [c_neg001]
    jb      .calc
    pop     rbx
    ret
.calc:
    movsd   xmm8, xmm0
    mulsd   xmm0, [c_two_pi_over_sr]
    movapd  xmm14, xmm0
    call    xu_dcos
    movapd  xmm11, xmm0
    movapd  xmm0, xmm14
    call    xu_dsin
    movapd  xmm12, xmm0
    movapd  xmm9, xmm15
    mulsd   xmm9, [c_two]
    movapd  xmm13, xmm12
    divsd   xmm13, xmm9                 ; alpha
    movsd   xmm10, [c_one]
    addsd   xmm10, xmm13
    movapd  xmm0, xmm13
    divsd   xmm0, xmm10
    movsd   [r10+BQ_B0], xmm0
    xorps   xmm3, xmm3
    movsd   [r10+BQ_B1], xmm3
    movapd  xmm0, xmm13
    xorps   xmm3, xmm3
    subsd   xmm3, xmm0
    divsd   xmm3, xmm10
    movsd   [r10+BQ_B2], xmm3
    movapd  xmm0, xmm11
    mulsd   xmm0, [c_neg_two]
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A1], xmm0
    movsd   xmm0, [c_one]
    subsd   xmm0, xmm13
    divsd   xmm0, xmm10
    movsd   [r10+BQ_A2], xmm0
    mov     dword [r10+BQ_KIND], 3
    movsd   [r10+BQ_FC], xmm8
    movsd   [r10+BQ_Q], xmm15
    pop     rbx
    ret

; --------------------------------------------------------------------------
;  xu_bq_run —— DF2T 差分方程，一次一个样本
;  rcx = XuBiquad*，xmm0 = x，返回 xmm0 = y
; --------------------------------------------------------------------------
global xu_bq_run
xu_bq_run:
    movsd   xmm1, [rcx+BQ_B0]
    mulsd   xmm1, xmm0
    addsd   xmm1, [rcx+BQ_Z1]           ; y = b0*x + z1
    movsd   xmm2, [rcx+BQ_B1]
    mulsd   xmm2, xmm0
    movsd   xmm3, [rcx+BQ_A1]
    mulsd   xmm3, xmm1
    subsd   xmm2, xmm3
    addsd   xmm2, [rcx+BQ_Z2]
    movsd   [rcx+BQ_Z1], xmm2
    movsd   xmm2, [rcx+BQ_B2]
    mulsd   xmm2, xmm0
    movsd   xmm3, [rcx+BQ_A2]
    mulsd   xmm3, xmm1
    subsd   xmm2, xmm3
    movsd   [rcx+BQ_Z2], xmm2
    movapd  xmm0, xmm1
    ret

; --------------------------------------------------------------------------
;  xu_noise —— 从循环噪声池取一个样本
;  rcx = XuVoice*，返回 double
; --------------------------------------------------------------------------
global xu_noise
xu_noise:
    mov     eax, [rcx+V_NPOS]
    cmp     eax, 8192
    jb      .ok
    xor     eax, eax
.ok:
    movss   xmm0, [rcx+V_NOISE+rax*4]
    cvtss2sd xmm0, xmm0
    inc     eax
    mov     [rcx+V_NPOS], eax
    ret

; ==========================================================================
;  xu_synth_sample —— 给定“年龄 t”返回该声部此刻的样本
;  rdi = XuVoice*，rsi = XuPadParams*，xmm0 = t（秒），返回 xmm0
; ==========================================================================
global xu_synth_sample
xu_synth_sample:
    PROLOG  192
    mov     r12, rdi
    mov     r13, rsi
    movsd   [rbp+L_T], xmm0
    mov     eax, [r12+V_PAD]
    lea     rdx, [rel synth_tab]
    movsxd  rax, dword [rdx+rax*4]
    add     rax, rdx
    jmp     rax

; ---------------- 0 底鼓 ----------------
synth_bd:
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_40]
    addsd   xmm0, [c_40]
    movsd   [rbp+L_T0], xmm0            ; fEnd
    LDF     xmm1, r13, PP_TONE
    mulsd   xmm1, [c_1_9]
    addsd   xmm1, [c_1_6]
    mulsd   xmm1, xmm0
    movsd   [rbp+L_T1], xmm1            ; fStart
    LDF     xmm2, r13, PP_DECAY
    movsd   xmm3, [c_one]
    subsd   xmm3, xmm2
    mulsd   xmm3, [c_0_012]
    addsd   xmm3, [c_0_018]
    movsd   [rbp+L_T2], xmm3            ; tau
    subsd   xmm1, xmm0
    movsd   [rbp+L_T3], xmm1            ; df
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm3
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    movsd   xmm1, [c_one]
    subsd   xmm1, xmm0                  ; 1-exp(-t/tau)
    mulsd   xmm1, [rbp+L_T3]
    mulsd   xmm1, [rbp+L_T2]
    movsd   xmm2, [rbp+L_T0]
    mulsd   xmm2, [rbp+L_T]
    addsd   xmm1, xmm2
    mulsd   xmm1, [c_two_pi]
    movapd  xmm0, xmm1
    call    xu_dsin
    LDF     xmm1, r13, PP_DECAY
    mulsd   xmm1, [c_1_2]
    addsd   xmm1, [c_1_15]
    mulsd   xmm0, xmm1
    call    xu_dtanh
    mulsd   xmm0, [c_092]
    movsd   [rbp+L_T4], xmm0            ; body
    LDF     xmm1, r13, PP_DECAY
    mulsd   xmm1, [c_1_48]
    addsd   xmm1, [c_0_12]
    cmp     dword [r12+V_ACCENT], 0
    je      .bd_noacc
    mulsd   xmm1, [c_118]
.bd_noacc:
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm1
    xorpd   xmm2, xmm2
    subsd   xmm2, xmm0
    movapd  xmm0, xmm2
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T4]
    movsd   [rbp+L_T4], xmm0            ; body*amp
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_3000]
    addsd   xmm0, [c_1800]
    movsd   xmm1, [c_0_8]
    lea     rcx, [r12+V_HP]
    call    xu_bq_hp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_HP]
    call    xu_bq_run
    movsd   [rbp+L_T5], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [c_0_0035]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T5]
    LDF     xmm1, r13, PP_TONE
    mulsd   xmm1, [c_0_32]
    addsd   xmm1, [c_0_06]
    mulsd   xmm0, xmm1
    addsd   xmm0, [rbp+L_T4]
    EPILOG

; ---------------- 1 军鼓 ----------------
synth_sd:
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_110]
    addsd   xmm0, [c_150]
    movsd   [rbp+L_T0], xmm0            ; f1
    LDF     xmm1, r13, PP_TONE
    mulsd   xmm1, [c_0_22]
    addsd   xmm1, [c_1_78]
    mulsd   xmm1, xmm0
    movsd   [rbp+L_T1], xmm1            ; f2
    LDF     xmm2, r13, PP_DECAY
    mulsd   xmm2, [c_0_14]
    addsd   xmm2, [c_0_055]
    movsd   [rbp+L_T2], xmm2            ; decO
    movsd   xmm0, [rbp+L_T0]
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_dsin
    mulsd   xmm0, [c_0_55]
    movsd   [rbp+L_T3], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [rbp+L_T2]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T3]
    movsd   [rbp+L_T3], xmm0            ; term1
    movsd   xmm0, [rbp+L_T1]
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_dsin
    mulsd   xmm0, [c_0_34]
    movsd   [rbp+L_T4], xmm0
    movsd   xmm1, [rbp+L_T2]
    mulsd   xmm1, [c_0_72]
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm1
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T4]
    addsd   xmm0, [rbp+L_T3]
    movsd   [rbp+L_T3], xmm0            ; osc
    LDF     xmm0, r13, PP_SNAPPY
    mulsd   xmm0, [c_3800]
    addsd   xmm0, [c_1200]
    LDF     xmm1, r13, PP_TONE
    mulsd   xmm1, [c_0_8]
    addsd   xmm1, [c_0_9]
    lea     rcx, [r12+V_BP]
    call    xu_bq_bp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_BP]
    call    xu_bq_run
    movsd   [rbp+L_T4], xmm0
    LDF     xmm1, r13, PP_DECAY
    mulsd   xmm1, [c_0_16]
    addsd   xmm1, [c_0_075]
    cmp     dword [r12+V_ACCENT], 0
    je      .sd_noacc
    mulsd   xmm1, [c_0_85]
.sd_noacc:
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm1
    xorpd   xmm2, xmm2
    subsd   xmm2, xmm0
    movapd  xmm0, xmm2
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T4]
    LDF     xmm1, r13, PP_SNAPPY
    mulsd   xmm1, [c_0_7]
    addsd   xmm1, [c_0_25]
    mulsd   xmm0, xmm1
    movsd   xmm1, [rbp+L_T3]
    mulsd   xmm1, [c_0_7]
    addsd   xmm0, xmm1
    EPILOG

; ---------------- 2..7 嗵鼓 / 康加 ----------------
synth_tom:
    mov     eax, [r12+V_PAD]
    sub     eax, 2
    mov     r14d, eax
    lea     rdx, [rel tom_base]
    movsxd  rax, eax
    movsd   xmm0, [rdx+rax*8]
    movsd   [rbp+L_T0], xmm0            ; baseF
    cmp     r14d, 3
    jae     .conga
    LDF     xmm0, r13, PP_DECAY
    mulsd   xmm0, [c_0_75]
    addsd   xmm0, [c_0_15]
    movsd   [rbp+L_T1], xmm0
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_0_22]
    addsd   xmm0, [c_0_04]
    movsd   [rbp+L_T2], xmm0
    movsd   xmm0, [c_1_9]
    movsd   [rbp+L_T3], xmm0
    movsd   xmm0, [c_0_035]
    movsd   [rbp+L_T4], xmm0
    jmp     .go
.conga:
    LDF     xmm0, r13, PP_DECAY
    mulsd   xmm0, [c_0_55]
    addsd   xmm0, [c_0_1]
    movsd   [rbp+L_T1], xmm0
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_0_12]
    addsd   xmm0, [c_0_02]
    movsd   [rbp+L_T2], xmm0
    movsd   xmm0, [c_1_35]
    movsd   [rbp+L_T3], xmm0
    movsd   xmm0, [c_0_020]
    movsd   [rbp+L_T4], xmm0
.go:
    LDF     xmm0, r13, PP_TUNE
    addsd   xmm0, [c_0_6]
    mulsd   xmm0, [rbp+L_T0]
    movsd   [rbp+L_T5], xmm0            ; f0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [rbp+L_T4]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    movsd   xmm1, [c_one]
    subsd   xmm1, xmm0
    mulsd   xmm1, [rbp+L_T5]
    movsd   xmm2, [rbp+L_T3]
    subsd   xmm2, [c_one]
    mulsd   xmm1, xmm2
    mulsd   xmm1, [rbp+L_T4]
    movsd   xmm2, [rbp+L_T5]
    mulsd   xmm2, [rbp+L_T]
    addsd   xmm1, xmm2
    mulsd   xmm1, [c_two_pi]
    movapd  xmm0, xmm1
    call    xu_dsin
    movsd   [rbp+L_T6], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [rbp+L_T1]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T6]
    mulsd   xmm0, [c_0_9]
    movsd   [rbp+L_T6], xmm0
    movsd   xmm0, [c_2500]
    movsd   xmm1, [c_0_7]
    lea     rcx, [r12+V_HP]
    call    xu_bq_hp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_HP]
    call    xu_bq_run
    movsd   [rbp+L_T7], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [c_0_004]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T7]
    mulsd   xmm0, [rbp+L_T2]
    addsd   xmm0, [rbp+L_T6]
    EPILOG

; ---------------- 8 边击 ----------------
synth_rs:
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_1400]
    addsd   xmm0, [c_1200]
    movsd   [rbp+L_T0], xmm0
    LDF     xmm0, r13, PP_DECAY
    mulsd   xmm0, [c_0_05]
    addsd   xmm0, [c_0_012]
    movsd   [rbp+L_T1], xmm0
    movsd   xmm0, [rbp+L_T0]
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_square
    movsd   [rbp+L_T2], xmm0
    movsd   xmm0, [rbp+L_T0]
    movsd   xmm1, [c_2_2]
    lea     rcx, [r12+V_BP]
    call    xu_bq_bp
    movsd   xmm0, [rbp+L_T2]
    lea     rcx, [r12+V_BP]
    call    xu_bq_run
    movsd   [rbp+L_T2], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [rbp+L_T1]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T2]
    mulsd   xmm0, [c_0_8]
    movsd   [rbp+L_T3], xmm0
    movsd   xmm0, [c_3000]
    movsd   xmm1, [c_0_8]
    lea     rcx, [r12+V_HP]
    call    xu_bq_hp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_HP]
    call    xu_bq_run
    movsd   [rbp+L_T4], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [c_0_004]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T4]
    mulsd   xmm0, [c_0_35]
    addsd   xmm0, [rbp+L_T3]
    EPILOG

; ---------------- 9 击木 ----------------
synth_cl:
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_1000]
    addsd   xmm0, [c_2000]
    movsd   [rbp+L_T0], xmm0
    LDF     xmm0, r13, PP_DECAY
    mulsd   xmm0, [c_0_035]
    addsd   xmm0, [c_0_018]
    movsd   [rbp+L_T1], xmm0
    movsd   xmm0, [rbp+L_T0]
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_dsin
    movsd   [rbp+L_T2], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [rbp+L_T1]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T2]
    mulsd   xmm0, [c_0_9]
    EPILOG

; ---------------- 10 拍手 ----------------
synth_cp:
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_1700]
    addsd   xmm0, [c_800]
    movsd   xmm1, [c_1_1]
    lea     rcx, [r12+V_BP]
    call    xu_bq_bp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_BP]
    call    xu_bq_run
    movsd   [rbp+L_T0], xmm0            ; nz
    xorpd   xmm0, xmm0
    movsd   [rbp+L_T1], xmm0            ; env = 0
    xor     r14d, r14d
.cp_loop:
    cmp     r14d, 4
    jge     .cp_done
    cvtsi2sd xmm0, r14d
    mulsd   xmm0, [c_0_009]
    movsd   xmm1, [rbp+L_T]
    comisd  xmm1, xmm0
    jb      .cp_next
    subsd   xmm1, xmm0
    divsd   xmm1, [c_0_011]
    xorpd   xmm2, xmm2
    subsd   xmm2, xmm1
    movapd  xmm0, xmm2
    call    xu_dexp
    mulsd   xmm0, [c_0_55]
    addsd   xmm0, [rbp+L_T1]
    movsd   [rbp+L_T1], xmm0
.cp_next:
    inc     r14d
    jmp     .cp_loop
.cp_done:
    LDF     xmm2, r13, PP_DECAY
    mulsd   xmm2, [c_0_35]
    addsd   xmm2, [c_0_05]
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm2
    xorpd   xmm2, xmm2
    subsd   xmm2, xmm0
    movapd  xmm0, xmm2
    call    xu_dexp
    LDF     xmm1, r13, PP_TAIL
    mulsd   xmm1, [c_0_55]
    addsd   xmm1, [c_0_15]
    mulsd   xmm0, xmm1
    addsd   xmm0, [rbp+L_T1]
    mulsd   xmm0, [rbp+L_T0]
    EPILOG

; ---------------- 11 沙锤 ----------------
synth_ma:
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_5000]
    addsd   xmm0, [c_4000]
    movsd   xmm1, [c_0_9]
    lea     rcx, [r12+V_HP]
    call    xu_bq_hp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_HP]
    call    xu_bq_run
    movsd   [rbp+L_T0], xmm0
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, [c_0_0015]
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    movsd   xmm1, [c_one]
    subsd   xmm1, xmm0                  ; 1-exp
    movsd   [rbp+L_T1], xmm1
    LDF     xmm2, r13, PP_DECAY
    mulsd   xmm2, [c_0_12]
    addsd   xmm2, [c_0_018]
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm2
    xorpd   xmm2, xmm2
    subsd   xmm2, xmm0
    movapd  xmm0, xmm2
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T1]
    mulsd   xmm0, [rbp+L_T0]
    mulsd   xmm0, [c_0_85]
    EPILOG

; ---------------- 12 牛铃 ----------------
synth_cb:
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_300]
    addsd   xmm0, [c_400]
    movsd   [rbp+L_T0], xmm0            ; f
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_square
    movsd   [rbp+L_T1], xmm0
    movsd   xmm0, [rbp+L_T0]
    mulsd   xmm0, [c_1_48]
    mulsd   xmm0, [rbp+L_T]
    mulsd   xmm0, [c_two_pi]
    call    xu_square
    movsd   [rbp+L_T2], xmm0
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_1200]
    addsd   xmm0, [c_2400]
    movsd   xmm1, [c_1_4]
    lea     rcx, [r12+V_BP]
    call    xu_bq_bp
    movsd   xmm0, [rbp+L_T1]
    mulsd   xmm0, [c_half]
    movsd   xmm1, [rbp+L_T2]
    mulsd   xmm1, [c_half]
    addsd   xmm0, xmm1
    lea     rcx, [r12+V_BP]
    call    xu_bq_run
    movsd   [rbp+L_T3], xmm0
    LDF     xmm2, r13, PP_DECAY
    mulsd   xmm2, [c_0_35]
    addsd   xmm2, [c_0_05]
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm2
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    mulsd   xmm0, [rbp+L_T3]
    mulsd   xmm0, [c_0_85]
    EPILOG

; ---------------- 13..15 镲族 ----------------
synth_metal:
    mov     r14d, [r12+V_PAD]
    LDF     xmm0, r13, PP_TUNE
    mulsd   xmm0, [c_0_5]
    addsd   xmm0, [c_0_75]
    mulsd   xmm0, [c_320]
    movsd   [rbp+L_T1], xmm0            ; root
    xorpd   xmm0, xmm0
    movsd   [rbp+L_T2], xmm0            ; metal = 0
    mov     r15d, 0
.m_loop:
    cmp     r15d, 6
    jge     .m_done
    movsxd  rax, r15d
    lea     rdx, [rel metal_ratios]
    movsd   xmm1, [rdx+rax*8]
    mulsd   xmm1, [rbp+L_T1]
    mulsd   xmm1, [rbp+L_T]
    mulsd   xmm1, [c_two_pi]
    movapd  xmm0, xmm1
    call    xu_square
    addsd   xmm0, [rbp+L_T2]
    movsd   [rbp+L_T2], xmm0
    inc     r15d
    jmp     .m_loop
.m_done:
    movsd   xmm0, [rbp+L_T2]
    divsd   xmm0, [c_6]
    movsd   [rbp+L_T2], xmm0
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_2600]
    addsd   xmm0, [c_5200]
    movsd   xmm1, [c_0_8]
    lea     rcx, [r12+V_HP]
    call    xu_bq_hp
    movsd   xmm0, [rbp+L_T2]
    lea     rcx, [r12+V_HP]
    call    xu_bq_run
    movsd   [rbp+L_T2], xmm0
    LDF     xmm0, r13, PP_TONE
    mulsd   xmm0, [c_3600]
    addsd   xmm0, [c_7600]
    movsd   xmm1, [c_0_9]
    lea     rcx, [r12+V_BP]
    call    xu_bq_bp
    movsd   xmm0, [rbp+L_T2]
    lea     rcx, [r12+V_BP]
    call    xu_bq_run
    movsd   [rbp+L_T2], xmm0
    ; 衰减时长按 pad 区分
    LDF     xmm1, r13, PP_DECAY
    cmp     r14d, 15
    je      .dec_ch
    cmp     r14d, 14
    je      .dec_oh
    mulsd   xmm1, [c_0_175]
    addsd   xmm1, [c_0_25]
    jmp     .dec_done
.dec_ch:
    mulsd   xmm1, [c_0_20]
    addsd   xmm1, [c_0_028]
    jmp     .dec_done
.dec_oh:
    mulsd   xmm1, [c_0_65]
    addsd   xmm1, [c_0_12]
.dec_done:
    movsd   [rbp+L_T3], xmm1
    movsd   xmm0, [rbp+L_T]
    divsd   xmm0, xmm1
    xorpd   xmm1, xmm1
    subsd   xmm1, xmm0
    movapd  xmm0, xmm1
    call    xu_dexp
    movsd   [rbp+L_T4], xmm0            ; env
    movsd   xmm0, [rbp+L_T2]
    mulsd   xmm0, [rbp+L_T4]
    movsd   [rbp+L_T5], xmm0            ; out
    cmp     r14d, 13
    jne     .m_fin
    movsd   xmm0, [c_10000]
    movsd   xmm1, [c_0_7]
    lea     rcx, [r12+V_BP2]
    call    xu_bq_bp
    mov     rcx, r12
    call    xu_noise
    lea     rcx, [r12+V_BP2]
    call    xu_bq_run
    mulsd   xmm0, [rbp+L_T4]
    mulsd   xmm0, [c_0_12]
    addsd   xmm0, [rbp+L_T5]
    movsd   [rbp+L_T5], xmm0
.m_fin:
    movsd   xmm0, [rbp+L_T5]
    mulsd   xmm0, [c_0_75]
    EPILOG

; 分派表：dd 相对偏移，避免绝对地址重定位
align 4
synth_tab:
    dd synth_bd - synth_tab
    dd synth_sd - synth_tab
    dd synth_tom - synth_tab
    dd synth_tom - synth_tab
    dd synth_tom - synth_tab
    dd synth_tom - synth_tab
    dd synth_tom - synth_tab
    dd synth_tom - synth_tab
    dd synth_rs - synth_tab
    dd synth_cl - synth_tab
    dd synth_cp - synth_tab
    dd synth_ma - synth_tab
    dd synth_cb - synth_tab
    dd synth_metal - synth_tab
    dd synth_metal - synth_tab
    dd synth_metal - synth_tab

; ==========================================================================
;  xu_voice_render —— 把一个 voice 在 [segStart, segEnd) 内混进 L/R
;  rcx=v, rdx=L, r8=R, r9d=n,
;  [rsp+32]=b0, [rsp+40]=segStart, [rsp+48]=segEnd, [rsp+56]=params
;  （Win64 约定：第 5 个参数在调用方 [rsp+32]；本函数 push rbp 后即 [rbp+48]）
; ==========================================================================
global xu_voice_render
xu_voice_render:
    PROLOG  192
    mov     r12, rcx                    ; v
    mov     r13, rdx                    ; L
    mov     r14, r8                     ; R
    mov     [rbp-64], r9                ; n
    mov     rbx, [rbp+48]               ; b0
    mov     rsi, [rbp+56]               ; segStart
    mov     rdi, [rbp+64]               ; segEnd
    mov     [rbp-72], rdi
    mov     rax, [rbp+72]               ; params
    mov     [rbp-80], rax
    ; a = max(segStart, v->start)
    mov     rax, [r12+V_START]
    cmp     rsi, rax
    jge     .a_ok
    mov     rsi, rax
.a_ok:
    mov     [rbp-88], rsi
    ; b = min(segEnd, v->stop)
    mov     rax, [r12+V_STOP]
    mov     rdx, [rbp-72]
    cmp     rdx, rax
    jle     .b_ok
    mov     rdx, rax
.b_ok:
    cmp     rdx, rsi
    jg      .have
    mov     rax, [r12+V_STOP]
    cmp     [rbp-72], rax
    jle     .ret0
    mov     dword [r12+V_ACTIVE], 0
.ret0:
    EPILOG
.have:
    mov     [rbp-96], rdx               ; b
    mov     rax, rdx
    sub     rax, rsi
    mov     [rbp-104], rax              ; m
    mov     rax, rsi
    sub     rax, rbx
    mov     [rbp-112], rax              ; off
    cmp     qword [rbp-104], 0
    jle     .ret0
    movsxd  rcx, dword [rbp-64]
    cmp     rax, rcx
    jge     .ret0
    ; 等功率声像
    mov     rsi, [rbp-80]
    LDF     xmm0, rsi, PP_PAN
    maxsd   xmm0, [c_zero]
    minsd   xmm0, [c_one]
    mulsd   xmm0, [c_half_pi]
    movapd  xmm15, xmm0
    call    xu_dcos
    movsd   [rbp-128], xmm0             ; lg
    movapd  xmm0, xmm15
    call    xu_dsin
    movsd   [rbp-136], xmm0             ; rg
    ; g = gain * level * 0.42
    movsd   xmm0, [r12+V_GAIN]
    LDF     xmm1, rsi, PP_LEVEL
    mulsd   xmm0, xmm1
    mulsd   xmm0, [c_42]
    movsd   [rbp-120], xmm0
    xor     r15, r15
.loop:
    cmp     r15, [rbp-104]
    jge     .done
    mov     rax, [rbp-88]
    add     rax, r15
    mov     [rbp-144], rax              ; abs
    sub     rax, [r12+V_START]
    cvtsi2sd xmm0, rax
    mulsd   xmm0, [c_inv_sr]
    mov     rdi, r12
    mov     rsi, [rbp-80]
    call    xu_synth_sample
    ; 掐断淡出
    mov     rax, [r12+V_STOP]
    mov     rdx, 0x7fffffffffffffff
    cmp     rax, rdx
    je      .nofade
    mov     rdx, [rbp-144]
    sub     rax, rdx                    ; dist
    movsxd  rcx, dword [r12+V_FADELEN]
    cmp     rax, rcx
    jge     .nofade
    test    rax, rax
    jle     .silent
    cvtsi2sd xmm1, rax
    cvtsi2sd xmm2, rcx
    divsd   xmm1, xmm2
    mulsd   xmm0, xmm1
    jmp     .nofade
.silent:
    xorpd   xmm0, xmm0
.nofade:
    mulsd   xmm0, [rbp-120]             ; * g
    movsd   xmm1, xmm0
    mulsd   xmm1, [rbp-128]             ; L
    mulsd   xmm0, [rbp-136]             ; R
    mov     rax, [rbp-112]
    add     rax, r15
    movss   xmm2, [r13+rax*4]
    cvtss2sd xmm2, xmm2
    addsd   xmm2, xmm1
    cvtsd2ss xmm2, xmm2
    movss   [r13+rax*4], xmm2
    movss   xmm2, [r14+rax*4]
    cvtss2sd xmm2, xmm2
    addsd   xmm2, xmm0
    cvtsd2ss xmm2, xmm2
    movss   [r14+rax*4], xmm2
    inc     r15
    jmp     .loop
.done:
    mov     rax, [rbp+64]
    sub     rax, [r12+V_START]
    cmp     rax, 192000
    jle     .ret
    mov     dword [r12+V_ACTIVE], 0
.ret:
    EPILOG
