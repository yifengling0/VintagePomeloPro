.text
.globl _guest_mul_loop
_guest_mul_loop:
 pushl %ebx
 pushl %esi
 pushl %edi
 movl 16(%esp),%esi
 movl 20(%esp),%edi
 movl 24(%esp),%ebx
 movl 28(%esp),%ecx
1:
 fldt (%esi)
 fldt (%edi)
 fmulp %st,%st(1)
 fstpt (%ebx)
 decl %ecx
 jnz 1b
 popl %edi
 popl %esi
 popl %ebx
 ret
