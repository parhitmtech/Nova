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
    mov  rbx, 35

    mov  rax, rbx
    mov  r13, rax
    call nova_f0
    mov  r12, rax
    mov  rax, r12
    mov  rbx, rax
    lea  rdi, [rel fmt_out_int]
    mov  rsi, rbx
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
    push rbx
    push r12
    push r13
    push r14
    push r15
    ; initialize constants
    mov  rsi, 2
    mov  rdx, 1
    mov  rcx, 2

    mov  rax, r13
    cmp  rax, rcx
    jge  .L0
    mov  rax, r13
    mov  rbx, rax
    mov  rax, rbx
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    leave
    ret
.L0:
    mov  rax, r13
    sub  rax, rdx
    mov  rbx, rax
    mov  rax, rbx
    mov  r13, rax
    call nova_f0
    mov  rbx, rax
    mov  rax, rbx
    mov  r12, rax
    mov  rax, r13
    sub  rax, rsi
    mov  rbx, rax
    mov  rax, rbx
    mov  r13, rax
    call nova_f0
    mov  rbx, rax
    mov  rax, rbx
    mov  rbx, rax
    mov  rax, r12
    add  rax, rbx
    mov  rbx, rax
    mov  rax, rbx
    mov  rbx, rax
    mov  rax, rbx
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    leave
    ret

