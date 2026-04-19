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
    sub  rsp, 400

    ; initialize constant slots
    mov  qword [rbp - 16], 20
    mov  qword [rbp - 208], 1
    mov  qword [rbp - 256], 1
    mov  qword [rbp - 280], 1
    mov  qword [rbp - 328], 1
    mov  qword [rbp - 344], 1
    mov  qword [rbp - 376], 1

    mov  rax, qword [rbp - 16]
    mov  qword [rbp - 8], rax
    mov  rax, qword [rbp - 192]
    mov  qword [rbp - 184], rax
.L1:
    mov  rax, qword [rbp - 184]
    cmp  rax, qword [rbp - 8]
    jge  .L0
    mov  rax, qword [rbp - 8]
    sub  rax, qword [rbp - 184]
    mov  qword [rbp - 200], rax
    mov  rdx, qword [rbp - 200]
    mov  rcx, qword [rbp - 184]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  [rbp + rax], rdx
    mov  rax, qword [rbp - 184]
    add  rax, qword [rbp - 208]
    mov  qword [rbp - 216], rax
    mov  rax, qword [rbp - 216]
    mov  qword [rbp - 184], rax
   jmp  .L1
.L0:
    mov  rax, qword [rbp - 232]
    mov  qword [rbp - 224], rax
.L7:
    mov  rax, qword [rbp - 224]
    cmp  rax, qword [rbp - 8]
    jge  .L2
    mov  rax, qword [rbp - 240]
    mov  qword [rbp - 184], rax
    mov  rax, qword [rbp - 8]
    sub  rax, qword [rbp - 256]
    mov  qword [rbp - 264], rax
    mov  rax, qword [rbp - 264]
    mov  qword [rbp - 248], rax
.L6:
    mov  rax, qword [rbp - 184]
    cmp  rax, qword [rbp - 248]
    jge  .L3
    mov  rax, qword [rbp - 184]
    add  rax, qword [rbp - 280]
    mov  qword [rbp - 288], rax
    mov  rax, qword [rbp - 288]
    mov  qword [rbp - 272], rax
    mov  rcx, qword [rbp - 184]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  rdx, [rbp + rax]
    mov  qword [rbp - 304], rdx
    mov  rax, qword [rbp - 304]
    mov  qword [rbp - 296], rax
    mov  rcx, qword [rbp - 272]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  rdx, [rbp + rax]
    mov  qword [rbp - 320], rdx
    mov  rax, qword [rbp - 320]
    mov  qword [rbp - 312], rax
    mov  rax, qword [rbp - 296]
    cmp  rax, qword [rbp - 312]
    jle  .L4
    mov  rdx, qword [rbp - 312]
    mov  rcx, qword [rbp - 184]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  [rbp + rax], rdx
    mov  rdx, qword [rbp - 296]
    mov  rcx, qword [rbp - 272]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  [rbp + rax], rdx
   jmp  .L5
.L4:
.L5:
    mov  rax, qword [rbp - 184]
    add  rax, qword [rbp - 328]
    mov  qword [rbp - 336], rax
    mov  rax, qword [rbp - 336]
    mov  qword [rbp - 184], rax
   jmp  .L6
.L3:
    mov  rax, qword [rbp - 224]
    add  rax, qword [rbp - 344]
    mov  qword [rbp - 352], rax
    mov  rax, qword [rbp - 352]
    mov  qword [rbp - 224], rax
   jmp  .L7
.L2:
    mov  rax, qword [rbp - 360]
    mov  qword [rbp - 184], rax
.L9:
    mov  rax, qword [rbp - 184]
    cmp  rax, qword [rbp - 8]
    jge  .L8
    mov  rcx, qword [rbp - 184]
    mov  rax, 2
    add  rax, rcx
    inc  rax
    imul rax, 8
    neg  rax
    mov  rdx, [rbp + rax]
    mov  qword [rbp - 368], rdx
    lea  rdi, [rel fmt_out_int]
    mov  rsi, qword [rbp - 368]
    xor  eax, eax
    call printf
    mov  rax, qword [rbp - 184]
    add  rax, qword [rbp - 376]
    mov  qword [rbp - 384], rax
    mov  rax, qword [rbp - 384]
    mov  qword [rbp - 184], rax
   jmp  .L9
.L8:

    ; exit
    xor  eax, eax
    leave
    ret
