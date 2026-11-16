; ==========================================================================
;  ui.asm —— XU-808 ASM 版控制台界面（Win32 Console）
;
;  署名：徐某。
;  为什么不是 GDI：汇编版把工程量集中在 DSP/音序器上，界面用控制台
;  既能演示全部实时能力（播放/停止、切 pattern、看播放头、导出 WAV），
;  又不需要几十个 GDI 调用和字体资源。外观吸收 808 的配色，但不画像素。
;
;  按键：
;    SPACE —— 播放 / 停止
;    N / P —— 下一个 / 上一个 Pattern
;    1..8  —— 直接选 Pattern
;    W     —— 导出 xu808_asm.wav（离线渲染，与实时同一套 DSP）
;    Q/ESC —— 退出（先停音频、再关 waveOut）
;
;  音频线程与 UI 线程彻底分离：UI 只读引擎里的播放头/电平做显示，
;  waveOut 回调在独立线程里渲染并回写第 5 块起的缓冲。
; ==========================================================================
%include "xu808.inc"

default rel

extern g_project
extern g_engine
extern g_voices
extern xu_engine_init
extern xu_engine_play
extern xu_engine_stop
extern xu_engine_request_pattern
extern xu_engine_trigger_pad
extern xu_audio_start
extern xu_audio_stop
extern xu_audio_running
extern xu_export_wav

extern AllocConsole
extern FreeConsole
extern GetStdHandle
extern SetConsoleTitleA
extern SetConsoleCursorPosition
extern WriteConsoleA
extern ReadConsoleInputA
extern WaitForSingleObject
extern ExitProcess

section .rdata
s_title:    db "XU-808 ASM - Xu Mou", 0
s_banner:   db "XU-808  RHYTHM COMPOSER  (x86-64 NASM)  by Xu Mou", 13, 10
            db "16 voices / 48kHz stereo / 4-buffer waveOut", 13, 10
            db "SPACE=Play/Stop  N/P=Pattern  1-8=Pattern  W=Export WAV  Q=Quit", 13, 10
S_BANNER_LEN equ $ - s_banner

s_st_pat:   db "  PAT ", 0
s_st_step:  db "  STEP ", 0
s_st_play:  db "  [PLAY]", 0
s_st_stop:  db "  [STOP]", 0
s_st_bpm:   db "  BPM ", 0
s_st_aud:   db "  AUDIO ", 0
s_st_on:    db "ON        ", 0
s_st_off:   db "NO DEVICE ", 0
s_msg_exp:  db "exporting xu808_asm.wav ...", 0
s_msg_ok:   db "export OK -> xu808_asm.wav            ", 0
s_msg_fail: db "export FAILED                          ", 0
s_msg_pat:  db "pattern switched                       ", 0
s_msg_play: db "transport: PLAY                         ", 0
s_msg_stop: db "transport: STOP                         ", 0
s_msg_bye:  db "bye.                                    ", 0
s_slash:    db "/", 0
s_blank:    db "                                        ", 0
s_spaces:   db "                                                                                ", 0

wavpath:    dw 'x','u','8','0','8','_','a','s','m','.','w','a','v',0

section .bss
alignb 16
g_hOut:     resq 1
g_hIn:      resq 1
g_audiook:  resd 1
g_nwritten: resd 1
g_rec:      resb 32
g_nread:    resd 1
lbuf:       resb 160
mbuf:       resb 96

section .text

; --------------------------------------------------------------------------
;  ui_app —— 把 rsi 指向的 NUL 结尾字符串追加到 rdi，返回推进后的 rdi
; --------------------------------------------------------------------------
ui_app:
    mov     al, [rsi]
    test    al, al
    jz      .done
    mov     [rdi], al
    inc     rdi
    inc     rsi
    jmp     ui_app
.done:
    mov     byte [rdi], 0
    ret

; ui_app2 —— eax = 0..99，追加两位（不足补 0）
ui_app2:
    mov     ecx, 10
    xor     edx, edx
    div     ecx
    add     al, '0'
    mov     [rdi], al
    mov     eax, edx
    add     al, '0'
    mov     [rdi+1], al
    mov     byte [rdi+2], 0
    add     rdi, 2
    ret

; ui_app3 —— eax = 0..999，追加三位
ui_app3:
    mov     ecx, 100
    xor     edx, edx
    div     ecx
    add     al, '0'
    mov     [rdi], al
    mov     eax, edx
    mov     ecx, 10
    xor     edx, edx
    div     ecx
    add     al, '0'
    mov     [rdi+1], al
    mov     eax, edx
    add     al, '0'
    mov     [rdi+2], al
    mov     byte [rdi+3], 0
    add     rdi, 3
    ret

; ui_pad —— 把 lbuf 补空格到 78 字符
ui_pad:
    lea     rax, [rel lbuf]
    mov     rcx, rdi
    sub     rcx, rax
    cmp     rcx, 78
    jge     .mark
.padloop:
    mov     byte [rdi], ' '
    inc     rdi
    inc     rcx
    cmp     rcx, 78
    jl      .padloop
.mark:
    mov     byte [rdi], 0
    ret

; --------------------------------------------------------------------------
;  ui_write —— rdx=ptr, r8d=len：在当前位置写（不换行）
; --------------------------------------------------------------------------
ui_write:
    sub     rsp, 40
    mov     rcx, [rel g_hOut]
    lea     r9, [rel g_nwritten]
    mov     qword [rsp+32], 0
    call    WriteConsoleA
    add     rsp, 40
    ret

; ui_setpos —— ecx=x, edx=y
ui_setpos:
    sub     rsp, 40
    mov     eax, edx
    shl     eax, 16
    or      eax, ecx
    mov     edx, eax
    mov     rcx, [rel g_hOut]
    call    SetConsoleCursorPosition
    add     rsp, 40
    ret

; ==========================================================================
;  xu_ui_run —— rcx = XuProject*
; ==========================================================================
global xu_ui_run
xu_ui_run:
    PROLOG  320
    mov     [rbp-64], rcx
    call    AllocConsole
    lea     rcx, [rel s_title]
    call    SetConsoleTitleA
    mov     ecx, -11
    call    GetStdHandle
    mov     [rel g_hOut], rax
    mov     ecx, -10
    call    GetStdHandle
    mov     [rel g_hIn], rax
    ; 引擎 + 音频
    lea     rcx, [rel g_engine]
    mov     rdx, [rbp-64]
    lea     r8, [rel g_voices]
    call    xu_engine_init
    lea     rcx, [rel g_engine]
    call    xu_audio_start
    mov     [rel g_audiook], eax
    ; 横幅
    lea     rdx, [rel s_banner]
    mov     r8d, S_BANNER_LEN
    call    ui_write
.main:
    ; ---------- 状态行 ----------
    lea     rdi, [rel lbuf]
    lea     rsi, [rel s_st_pat]
    call    ui_app
    mov     rax, [rbp-64]
    mov     eax, [rax+PJ_CURRENT]
    inc     eax
    call    ui_app2
    lea     rsi, [rel s_st_step]
    call    ui_app
    mov     eax, [rel g_engine+EN_STEPINDEX]
    inc     eax
    call    ui_app2
    lea     rsi, [rel s_slash]
    call    ui_app
    mov     rax, [rbp-64]
    mov     ecx, [rax+PJ_CURRENT]
    movsxd  rcx, ecx
    imul    rcx, rcx, PAT_SIZE
    add     rcx, rax
    mov     eax, [rcx+PAT_STEPS]
    call    ui_app2
    cmp     dword [rel g_engine+EN_PLAYING], 0
    je      .st_stop
    lea     rsi, [rel s_st_play]
    jmp     .st_play
.st_stop:
    lea     rsi, [rel s_st_stop]
.st_play:
    call    ui_app
    lea     rsi, [rel s_st_bpm]
    call    ui_app
    mov     rax, [rbp-64]
    movss   xmm0, [rax+PJ_MASTER+MS_BPM]
    cvttss2si eax, xmm0
    call    ui_app3
    lea     rsi, [rel s_st_aud]
    call    ui_app
    cmp     dword [rel g_audiook], 0
    je      .aud_off
    lea     rsi, [rel s_st_on]
    jmp     .aud_done
.aud_off:
    lea     rsi, [rel s_st_off]
.aud_done:
    call    ui_app
    call    ui_pad
    xor     ecx, ecx
    mov     edx, 3
    call    ui_setpos
    lea     rdx, [rel lbuf]
    mov     r8d, 78
    call    ui_write
    ; ---------- 等按键（不阻塞 50ms） ----------
    mov     rcx, [rel g_hIn]
    mov     edx, 50
    call    WaitForSingleObject
    test    eax, eax
    jnz     .main
    mov     rcx, [rel g_hIn]
    lea     rdx, [rel g_rec]
    mov     r8d, 1
    lea     r9, [rel g_nread]
    call    ReadConsoleInputA
    cmp     word [rel g_rec], 1
    jne     .main
    cmp     dword [rel g_rec+4], 0
    je      .main
    movzx   eax, word [rel g_rec+10]
    ; ---------- 命令 ----------
    cmp     al, 0x20                    ; SPACE
    je      .cmd_space
    cmp     al, 0x1B                    ; ESC
    je      .quit
    cmp     al, 'Q'
    je      .quit
    cmp     al, 'q'
    je      .quit
    cmp     al, 'N'
    je      .cmd_next
    cmp     al, 'n'
    je      .cmd_next
    cmp     al, 'P'
    je      .cmd_prev
    cmp     al, 'p'
    je      .cmd_prev
    cmp     al, 'W'
    je      .cmd_wav
    cmp     al, 'w'
    je      .cmd_wav
    cmp     al, '1'
    jl      .main
    cmp     al, '8'
    jg      .main
    sub     eax, '1'
    mov     edx, eax
    lea     rcx, [rel g_engine]
    call    xu_engine_request_pattern
    lea     rsi, [rel s_msg_pat]
    call    ui_message
    jmp     .main
.cmd_space:
    cmp     dword [rel g_engine+EN_PLAYING], 0
    je      .do_play
    lea     rcx, [rel g_engine]
    call    xu_engine_stop
    lea     rsi, [rel s_msg_stop]
    call    ui_message
    jmp     .main
.do_play:
    lea     rcx, [rel g_engine]
    call    xu_engine_play
    ; 播放时顺手打一下底鼓，确认音频链路是通的
    lea     rcx, [rel g_engine]
    xor     edx, edx
    mov     r8d, 110
    xor     r9d, r9d
    call    xu_engine_trigger_pad
    lea     rsi, [rel s_msg_play]
    call    ui_message
    jmp     .main
.cmd_next:
    mov     rax, [rbp-64]
    mov     edx, [rax+PJ_CURRENT]
    inc     edx
    and     edx, 15
    lea     rcx, [rel g_engine]
    call    xu_engine_request_pattern
    lea     rsi, [rel s_msg_pat]
    call    ui_message
    jmp     .main
.cmd_prev:
    mov     rax, [rbp-64]
    mov     edx, [rax+PJ_CURRENT]
    dec     edx
    and     edx, 15
    lea     rcx, [rel g_engine]
    call    xu_engine_request_pattern
    lea     rsi, [rel s_msg_pat]
    call    ui_message
    jmp     .main
.cmd_wav:
    lea     rsi, [rel s_msg_exp]
    call    ui_message
    mov     rax, [rbp-64]
    mov     r8d, [rax+PJ_MASTER+MS_SONGMODE]
    lea     rcx, [rel g_project]
    lea     rdx, [rel wavpath]
    mov     r9d, -1
    call    xu_export_wav
    test    eax, eax
    jz      .wav_fail
    lea     rsi, [rel s_msg_ok]
    jmp     .wav_msg
.wav_fail:
    lea     rsi, [rel s_msg_fail]
.wav_msg:
    call    ui_message
    jmp     .main
.quit:
    lea     rcx, [rel g_engine]
    call    xu_engine_stop
    call    xu_audio_stop
    lea     rsi, [rel s_msg_bye]
    call    ui_message
    call    FreeConsole
    xor     eax, eax
    EPILOG

; --------------------------------------------------------------------------
;  ui_message —— rsi = 消息（定长 78 字符，写到第 5 行）
; --------------------------------------------------------------------------
ui_message:
    PROLOG  96
    mov     rsi, rsi
    lea     rdi, [rel mbuf]
    call    ui_app
    ; 补空格
    lea     rax, [rel mbuf]
    mov     rcx, rdi
    sub     rcx, rax
.pad:
    cmp     rcx, 78
    jge     .done
    mov     byte [rdi], ' '
    inc     rdi
    inc     rcx
    jmp     .pad
.done:
    mov     byte [rdi], 0
    xor     ecx, ecx
    mov     edx, 5
    call    ui_setpos
    lea     rdx, [rel mbuf]
    mov     r8d, 78
    call    ui_write
    EPILOG
