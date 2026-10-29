// Owned static Darwin PID1 probe. No libc/dyld or Apple user binaries.
// Darwin ARM64 UNIX syscalls: getpid20, open5, write4, sysctl202, select93.
.text
.p2align 2
.global _start
_start:
 mov x9, sp
 and x9, x9, #-16
 mov sp, x9
 mov x16, #20
 svc #0x80
 b.cs L_bad_pid
 cmp x0, #1
 b.ne L_bad_pid
 mov x20, #1
 b L_open
L_bad_pid:
 mov x20, #0
L_open:
 adrp x0, console_path@PAGE
 add x0, x0, console_path@PAGEOFF
 mov x1, #1
 mov x2, #0
 mov x16, #5
 svc #0x80
 b.cs L_fallback
 mov x19, x0
 b L_hello
L_fallback:
 mov x19, #1
L_hello:
 cbz x20, L_fail
 adrp x1, hello@PAGE
 add x1, x1, hello@PAGEOFF
 bl L_write
 adrp x0, page_mib@PAGE
 add x0, x0, page_mib@PAGEOFF
 mov x1, #2
 adrp x2, page_size@PAGE
 add x2, x2, page_size@PAGEOFF
 adrp x3, page_length@PAGE
 add x3, x3, page_length@PAGEOFF
 mov x4, #0
 mov x5, #0
 mov x16, #202
 svc #0x80
 b.cs L_fail
 cbnz x0, L_fail
 adrp x9, page_size@PAGE
 ldr x9, [x9, page_size@PAGEOFF]
 cmp x9, #0x4000
 b.ne L_fail
 adrp x1, abi_ok@PAGE
 add x1, x1, abi_ok@PAGEOFF
 bl L_write
L_heartbeat:
 adrp x1, heartbeat@PAGE
 add x1, x1, heartbeat@PAGEOFF
 bl L_write
L_sleep:
 adrp x4, sleep_time@PAGE
 add x4, x4, sleep_time@PAGEOFF
 mov x9, #1
 stp x9, xzr, [x4]
 mov x0, #0
 mov x1, #0
 mov x2, #0
 mov x3, #0
 mov x16, #93
 svc #0x80
 b.cc L_heartbeat
 cmp x0, #4
 b.eq L_sleep
 b L_fail
L_write:
 mov x2, #0
L_length:
 ldrb w9, [x1, x2]
 cbz w9, L_write_ready
 add x2, x2, #1
 b L_length
L_write_ready:
 mov x0, x19
 mov x16, #4
 svc #0x80
 ret
L_fail:
 adrp x1, failed@PAGE
 add x1, x1, failed@PAGEOFF
 bl L_write
L_failed_idle:
 yield
 b L_failed_idle

.section __TEXT,__const
console_path: .asciz "/dev/console"
hello: .asciz "X9 PID1: EL0 entered; getpid=1\n"
hello_end:
abi_ok: .asciz "X9 PID1: hw.pagesize=16384; syscall ABI OK\n"
abi_ok_end:
heartbeat: .asciz "X9 PID1: heartbeat\n"
heartbeat_end:
failed: .asciz "X9 PID1: probe failed; inspect kernel log\n"
failed_end:
.p2align 2
page_mib: .long 6, 7
.data
.p2align 3
page_size: .quad 0
page_length: .quad 8
sleep_time: .quad 1, 0
