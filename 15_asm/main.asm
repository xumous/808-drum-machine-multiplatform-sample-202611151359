; ==========================================================================
;  main.asm —— XU-808 ASM 版入口（x86-64 NASM，无 CRT）
;
;  署名：徐某。
;  因为没有链接 C 运行时，入口不是 mainCRTStartup 而是自定义的
;  WinMainCRTStartup；命令行用 GetCommandLineA 自己扫。
;    · 带 --selftest：默认工程 → 离线渲染 16 步 → selftest_asm.wav
;      → 写 selftest_asm.txt（SELFTEST OK / FAIL）→ ExitProcess；
;    · 否则：创建窗口 + waveOut 实时演奏。
; ==========================================================================
%include "xu808.inc"

default rel

extern g_project
extern xu_project_default
extern xu_export_wav
extern xu_ui_run

extern ExitProcess
extern GetCommandLineA
extern CreateFileW
extern WriteFile
extern CloseHandle

section .rdata
wavpath:    dw 's','e','l','f','t','e','s','t','_','a','s','m','.','w','a','v',0
txtpath:    dw 's','e','l','f','t','e','s','t','_','a','s','m','.','t','x','t',0
s_ok:       db "SELFTEST OK", 13, 10
s_fail:     db "SELFTEST FAIL", 13, 10

section .bss
written:    resb 16

section .text
global WinMainCRTStartup
WinMainCRTStartup:
    sub     rsp, 88
    call    GetCommandLineA
    mov     rdi, rax
    call    has_selftest
    test    eax, eax
    jnz     .selftest
    ; ---------------- 图形界面 ----------------
    lea     rcx, [rel g_project]
    call    xu_project_default
    lea     rcx, [rel g_project]
    call    xu_ui_run
    xor     ecx, ecx
    call    ExitProcess
.selftest:
    lea     rcx, [rel g_project]
    call    xu_project_default
    lea     rcx, [rel g_project]
    lea     rdx, [rel wavpath]
    xor     r8d, r8d
    mov     r9d, -1
    call    xu_export_wav
    mov     ebx, eax
    ; 结果写文件（构建机无人值守也要能判断）
    lea     rcx, [rel txtpath]
    mov     edx, 0x40000000
    xor     r8d, r8d
    xor     r9d, r9d
    mov     qword [rsp+32], 2
    mov     qword [rsp+40], 0x80
    mov     qword [rsp+48], 0
    call    CreateFileW
    cmp     rax, -1
    je      .exit
    mov     r12, rax
    mov     rcx, rax
    lea     rdx, [rel s_ok]
    mov     r8d, 13
    test    ebx, ebx
    jnz     .wr
    lea     rdx, [rel s_fail]
    mov     r8d, 15
.wr:
    lea     r9, [rel written]
    mov     qword [rsp+32], 0
    call    WriteFile
    mov     rcx, r12
    call    CloseHandle
.exit:
    mov     ecx, ebx
    xor     ecx, 1
    call    ExitProcess

; --------------------------------------------------------------------------
;  命令行动手扫 "--selftest"
;  rdi = cmdline → eax
; --------------------------------------------------------------------------
has_selftest:
.loop:
    mov     al, [rdi]
    test    al, al
    jz      .no
    cmp     dword [rdi], 0x65732D2D       ; "--se"
    jne     .next
    cmp     dword [rdi+4], 0x6574666C     ; "lfte"
    jne     .next
    cmp     word [rdi+8], 0x7473          ; "st"
    jne     .next
    mov     eax, 1
    ret
.next:
    inc     rdi
    jmp     .loop
.no:
    xor     eax, eax
    ret
