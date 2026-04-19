section .data
    fmt_out_int  db "%d", 10, 0
    fmt_out_str  db "%s", 0
    fmt_in_int   db "%d", 0

section .text
    extern printf
    extern scanf
    global main

main:
    push rbp
    mov  rbp, rsp
    sub  rsp, 112

    ; initialize constant slots
    mov  qword [rbp - 24], 2
    mov  qword [rbp - 32], 1
    mov  qword [rbp - 56], 2
    mov  qword [rbp - 88], 35

    mov  rax, qword [rbp - 88]
    mov  qword [rbp - 8], rax
    call nova_f0
    mov  qword [rbp - 16], rax
    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 96], rax
    lea  rdi, [rel fmt_out_int]
    mov  rsi, qword [rbp - 96]
    xor  eax, eax
    call printf

    ; exit
    xor  eax, eax
    leave
    ret

; ── function nova_f0 ─────────────────────────────────
nova_f0:
    push rbp
    mov  rbp, rsp
    sub  rsp, 112

    mov  rax, qword [rbp - 8]
    cmp  rax, qword [rbp - 24]
    jge  .L0
    mov  rax, qword [rbp - 8]
    mov  qword [rbp - 16], rax
    mov  rax, qword [rbp - 16]
    leave
    ret
.L0:
    mov  rax, qword [rbp - 8]
    sub  rax, qword [rbp - 32]
    mov  qword [rbp - 40], rax
    mov  rax, qword [rbp - 40]
    mov  qword [rbp - 8], rax
    call nova_f0
    mov  qword [rbp - 16], rax
    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 48], rax
    mov  rax, qword [rbp - 8]
    sub  rax, qword [rbp - 56]
    mov  qword [rbp - 64], rax
    mov  rax, qword [rbp - 64]
    mov  qword [rbp - 8], rax
    call nova_f0
    mov  qword [rbp - 16], rax
    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 72], rax
    mov  rax, qword [rbp - 48]
    add  rax, qword [rbp - 72]
    mov  qword [rbp - 80], rax
    mov  rax, qword [rbp - 80]
    mov  qword [rbp - 16], rax
    mov  rax, qword [rbp - 16]
    leave
    ret

