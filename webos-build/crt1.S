/* Minimal ARM crt1.o targeting glibc 2.4+ __libc_start_main ABI */
    .text
    .align 2
    .global _start
    .type _start, %function
_start:
    mov fp, #0
    mov lr, #0

    /* r0 = rtld_fini (from dynamic linker) */
    mov ip, r0

    /* Pop argc and get argv from stack */
    ldr r1, [sp]        /* argc */
    add r2, sp, #4      /* argv = sp + 4 */

    /* Align stack to 8 bytes */
    bic sp, sp, #7

    /* Push stack_end, rtld_fini, fini=NULL on stack.
     * Three words would leave sp 4 mod 8 at the call; AAPCS needs 8-byte
     * alignment or variadic doubles/int64s are read off by a word. */
    mov r3, sp
    sub sp, sp, #4      /* padding */
    push {r3}           /* stack_end */
    push {ip}           /* rtld_fini */
    mov r3, #0
    push {r3}           /* fini = NULL (constructors via .init_array) */

    /* r0 = main, r1 = argc, r2 = argv, r3 = init = NULL */
    ldr r0, =main
    mov r3, #0          /* init = NULL (constructors via .init_array) */

    bl __libc_start_main

    bl abort

    .size _start, .-_start
