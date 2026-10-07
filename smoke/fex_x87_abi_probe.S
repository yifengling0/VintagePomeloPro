.text
.macro X87_PROBE name, instruction
.globl _\name
_\name:
  pushl %ebp
  pushl %ebx
  pushl %esi
  pushl %edi
  fninit
  fldcw 28(%esp)
  movl 20(%esp), %eax
  fldt (%eax)
  movl 24(%esp), %eax
  fldt (%eax)
  movdqu _x87_patterns+0, %xmm0
  movdqu _x87_patterns+16, %xmm1
  movdqu _x87_patterns+32, %xmm2
  movdqu _x87_patterns+48, %xmm3
  movdqu _x87_patterns+64, %xmm4
  movdqu _x87_patterns+80, %xmm5
  movdqu _x87_patterns+96, %xmm6
  movdqu _x87_patterns+112, %xmm7
  movl $0x10203040, %eax
  movl $0x21314151, %ecx
  movl $0x32425262, %edx
  movl $0x43536373, %ebx
  movl $0x54647484, %ebp
  movl $0x65758595, %esi
  movl $0x768696a6, %edi
  pushl $0x8d7
  popfl
  \instruction %st, %st(1)
  pushfl
  pushal
  movl 68(%esp), %eax
  fstpt 0(%eax)
  fnstsw 10(%eax)
  fnstcw 12(%eax)
  stmxcsr 16(%eax)
  movl 32(%esp), %ecx
  movl %ecx, 20(%eax)
  .set off, 0
  .rept 8
    movl off(%esp), %ecx
    movl %ecx, 24+off(%eax)
    .set off, off+4
  .endr
  movdqu %xmm0, 56(%eax)
  movdqu %xmm1, 72(%eax)
  movdqu %xmm2, 88(%eax)
  movdqu %xmm3, 104(%eax)
  movdqu %xmm4, 120(%eax)
  movdqu %xmm5, 136(%eax)
  movdqu %xmm6, 152(%eax)
  movdqu %xmm7, 168(%eax)
  addl $36, %esp
  popl %edi
  popl %esi
  popl %ebx
  popl %ebp
  ret
.endm
X87_PROBE x87_add, faddp
X87_PROBE x87_sub, fsubp
X87_PROBE x87_mul, fmulp
X87_PROBE x87_div, fdivp
