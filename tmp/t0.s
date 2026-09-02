	.text
.globl main
main:
	pushq %rbp
	movq %rsp, %rbp
	subq $0, %rsp
.L0_0:
	leaq .Lstr_0_0(%rip), %rdi
	movq $42, %rsi
	xorl %eax, %eax
	call printf@PLT
	movq $0, %rax
	jmp .L0_100000
.L0_100000:
	leave
	ret

	.section .rodata
.Lnegmask64:
	.quad 0x8000000000000000
	.quad 0
.Lnegmask32:
	.long 0x80000000
	.long 0
.Lstr_0_0:
	.string "%d\n"
