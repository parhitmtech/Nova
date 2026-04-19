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
    sub  rsp, 64

    ; initialize constant slots
    mov  qword [rbp - 16], 5
    mov  qword [rbp - 32], 3

    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 8], rax
    mov  rax, qword [rbp - 32]
    mov  qword [rbp - 24], rax
    mov  rax, qword [rbp - 8]
    add  rax, qword [rbp - 24]
    mov  qword [rbp - 48], rax
    mov  rax, qword [rbp - 48]
    mov  qword [rbp - 40], rax
    lea  rdi, [rel fmt_out_int]
    mov  rsi, qword [rbp - 40]
    xor  eax, eax
    call printf

    ; exit
    xor  eax, eax
    leave
    ret
