section .data
    fmt_out_int  db "%d", 10, 0
    fmt_out_str  db "%s", 0
    fmt_in_int   db "%d", 0
    strlit_0 db 100, 111, 110, 101, 10, 0

section .text
    extern printf
    extern scanf
    global main

main:
    push rbp
    mov  rbp, rsp
    sub  rsp, 80

    ; initialize constant slots
    mov  qword [rbp - 16], 32

    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 8], rax
    ; unhandled IR type 1016
    mov  rax, qword [rbp - 32]
    mov  qword [rbp - 24], rax
    ; unhandled IR type 1016
    mov  rax, qword [rbp - 48]
    mov  qword [rbp - 40], rax
    ; unhandled IR type 1016
    mov  rax, qword [rbp - 64]
    mov  qword [rbp - 56], rax
    lea  rdi, [rel strlit_0]
    xor  eax, eax
    call printf

    ; exit
    xor  eax, eax
    leave
    ret
