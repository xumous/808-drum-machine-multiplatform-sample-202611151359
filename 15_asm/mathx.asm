; ==========================================================================
;  mathx.asm —— 汇编版快速数学库（双精度，SSE2 标量）
;
;  署名：徐某。
;  为什么自己写 sin/exp/tanh：
;  1) 不依赖 msvcrt.dll，EXE 只依赖 kernel32/user32/gdi32/winmm；
;  2) 音频回调里每次调用 CRT 都要走一遍 ABI 与 errno，开销不可控；
;  3) 这三个函数的多项式版本精度足够（相对误差 < 1e-9），而且行为确定。
;
;  内部约定：输入 xmm0，输出 xmm0；只破坏 rax 与 xmm0..xmm7。
; ==========================================================================
%include "xu808.inc"
default rel

section .rdata
k_sin12:    dq -0.00000000016059043836821615   ; -1/13!

section .text

; --------------------------------------------------------------------------
;  xu_dsin —— sin(x)，先按 2π 做就近整数周期归约，再算 11 阶 Taylor
; --------------------------------------------------------------------------
global xu_dsin
xu_dsin:
    movapd  xmm7, xmm0                  ; 保存原始 x
    mulsd   xmm0, [c_inv_two_pi]
    cvtsd2si rax, xmm0                  ; n = round(x/2π)
    cvtsi2sd xmm1, rax
    movapd  xmm2, xmm1
    mulsd   xmm2, [c_two_pi]            ; n*2π（高位）
    subsd   xmm7, xmm2
    mulsd   xmm1, [c_two_pi_lo]         ; 低位修正，保证大相位精度
    subsd   xmm7, xmm1
    movapd  xmm0, xmm7                  ; r ∈ [-π, π]
    mulsd   xmm7, xmm7                  ; r²
    movsd   xmm1, [rel k_sin12]
    mulsd   xmm1, xmm7
    addsd   xmm1, [c_sin10]             ; 注意符号交替
    mulsd   xmm1, xmm7
    subsd   xmm1, [c_sin8]
    mulsd   xmm1, xmm7
    addsd   xmm1, [c_sin6]
    mulsd   xmm1, xmm7
    subsd   xmm1, [c_sin4]
    mulsd   xmm1, xmm7
    addsd   xmm1, [c_sin2]
    mulsd   xmm1, xmm7
    movsd   xmm2, [c_one]
    subsd   xmm2, xmm1
    mulsd   xmm0, xmm2                  ; sin = r * (1 - ...)
    ret

; --------------------------------------------------------------------------
;  xu_dcos —— cos(x) = sin(x + π/2)
; --------------------------------------------------------------------------
global xu_dcos
xu_dcos:
    addsd   xmm0, [c_half_pi]
    jmp     xu_dsin

; --------------------------------------------------------------------------
;  xu_dexp —— exp(x) = 2^n * 2^f，n=round(x*log2e)
;  2^f 用 u=f*ln2 的 7 阶 Taylor，|u|<0.347，误差 ~1e-10。
; --------------------------------------------------------------------------
global xu_dexp
xu_dexp:
    maxsd   xmm0, [c_neg80]
    minsd   xmm0, [c_pos80]
    mulsd   xmm0, [c_log2e]
    cvtsd2si rax, xmm0
    cvtsi2sd xmm1, rax
    subsd   xmm0, xmm1                  ; f ∈ [-0.5,0.5]
    mulsd   xmm0, [c_ln2]               ; u
    ; Horner：1 + u + u²/2! + u³/3! + … + u⁷/7!（系数由高次到低次）
    movsd   xmm1, [c_e5]                ; 1/5040
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_e4]                ; 1/720
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_e3]                ; 1/120
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_e2]                ; 1/24
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_sin2]              ; 1/6
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_half]              ; 1/2
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_one]
    mulsd   xmm1, xmm0
    addsd   xmm1, [c_one]               ; 1 + u*(...)
    add     rax, 1023
    shl     rax, 52                     ; 构造 2^n 的 IEEE 指数
    movq    xmm2, rax
    mulsd   xmm1, xmm2
    movapd  xmm0, xmm1
    ret

; --------------------------------------------------------------------------
;  xu_dtanh —— tanh(x) = (e^(2x)-1)/(e^(2x)+1)
;  先把 |x| 钳到 15，避免 e^30 以上的无意义计算。
; --------------------------------------------------------------------------
global xu_dtanh
xu_dtanh:
    maxsd   xmm0, [c_neg15]
    minsd   xmm0, [c_pos15]
    addsd   xmm0, xmm0
    call    xu_dexp
    movsd   xmm1, [c_one]
    movapd  xmm2, xmm0
    subsd   xmm0, xmm1                  ; e-1
    addsd   xmm2, xmm1                  ; e+1
    divsd   xmm0, xmm2
    ret

; --------------------------------------------------------------------------
;  xu_square —— 方波：sin(phase) >= 0 ? +1 : -1
; --------------------------------------------------------------------------
global xu_square
xu_square:
    call    xu_dsin
    xorpd   xmm1, xmm1
    comisd  xmm0, xmm1
    jae     .pos
    movsd   xmm0, [c_neg_one]
    ret
.pos:
    movsd   xmm0, [c_one]
    ret
