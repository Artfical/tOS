.intel_syntax noprefix
.section .text
.global _start
.type _start, @function
_start:
    cmp dword ptr [esp], 2
    jne fail
    mov eax, dword ptr [esp + 4]
    test eax, eax
    jz fail
    cmp byte ptr [eax], '/'
    jne fail
    mov eax, dword ptr [esp + 8]
    cmp byte ptr [eax], 'k'
    je parent_case
    cmp byte ptr [eax], 's'
    je check_stack
    cmp byte ptr [eax], 'e'
    jne fail
    jmp check_stack

parent_case:
    cmp byte ptr [eax + 1], 'e'
    jne fail

check_stack:
    cmp dword ptr [esp + 12], 0
    jne fail
    mov eax, dword ptr [esp + 16]
    cmp byte ptr [eax], 'T'
    jne fail
    cmp byte ptr [eax + 1], 'O'
    jne fail
    cmp byte ptr [eax + 2], 'S'
    jne fail
    cmp dword ptr [esp + 20], 0
    jne fail
    cmp dword ptr [esp + 24], 3      /* AT_PHDR */
    jne fail
    mov eax, dword ptr [esp + 28]
    test eax, eax
    jz fail
    cmp dword ptr [esp + 48], 6      /* AT_PAGESZ */
    jne fail
    cmp dword ptr [esp + 52], 4096
    jne fail

    mov eax, dword ptr [esp + 8]
    cmp byte ptr [eax], 'k'
    je spawn_child
    cmp byte ptr [eax], 's'
    je exec_child
    jmp test_mmap

spawn_child:
    /* Ask the kernel to copy user argv/envp while spawning this same ELF. */
    mov eax, 251
    mov ebx, offset child_path
    mov ecx, offset child_argv
    mov edx, offset child_envp
    int 0x80
    cmp eax, 0xfffff001
    jae fail
    mov esi, eax
    mov eax, 7              /* waitpid(child_pid, NULL, 0) */
    mov ebx, esi
    mov ecx, offset wait_status
    xor edx, edx
    int 0x80
    cmp eax, esi
    jne fail
    cmp dword ptr [wait_status], 0
    jne fail
    jmp exit_ok

exec_child:
    /* Replace this image with execve; a return indicates a failed exec. */
    mov eax, 11
    mov ebx, offset child_path
    mov ecx, offset exec_argv
    mov edx, offset exec_envp
    int 0x80
    jmp fail

test_mmap:
    /* set_thread_area/get_thread_area and a real ring-3 GS base access. */
    mov dword ptr [tls_desc], -1
    mov eax, 243             /* set_thread_area */
    mov ebx, offset tls_desc
    int 0x80
    test eax, eax
    jne fail
    mov eax, dword ptr [tls_desc]
    shl eax, 3
    or eax, 3
    mov gs, ax
    mov eax, dword ptr gs:[0]
    cmp eax, 0x746f5354
    jne fail
    mov eax, 244             /* get_thread_area */
    mov ebx, offset tls_desc
    int 0x80
    test eax, eax
    jne fail
    mov ax, 0x23             /* drop test TLS segment before exit */
    mov gs, ax

    /* clone a shared-address-space Linux thread; child returns with EAX=0. */
    mov dword ptr [clone_tid], 0
    mov dword ptr [clone_seen], 0
    mov eax, 120             /* clone(share VM/files/sighand/thread/TLS, ...) */
    mov ebx, 0x013d0f00
    mov ecx, offset clone_stack + 4096
    mov edx, offset clone_tid
    mov esi, offset clone_tls_desc
    mov edi, offset clone_tid
    int 0x80
    cmp eax, 0xfffff001
    jae fail
    test eax, eax
    jz clone_child
clone_parent:
    mov eax, dword ptr [clone_tid]
    test eax, eax
    jz clone_done
    mov eax, 240             /* futex(clone_tid, FUTEX_WAIT_PRIVATE, tid) */
    mov ebx, offset clone_tid
    mov ecx, 0x80
    mov edx, dword ptr [clone_tid]
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    int 0x80
    cmp eax, 0xfffffff5      /* EAGAIN means child won the wake race */
    je clone_parent
    test eax, eax
    jne fail
    cmp dword ptr [clone_tid], 0
    jne clone_parent
clone_done:
    cmp dword ptr [clone_tid], 0
    jne fail
    cmp dword ptr [clone_seen], 0x54485244
    jne fail
    jmp after_clone

clone_child:
    mov eax, dword ptr gs:[0]
    cmp eax, 0x746f5354
    jne clone_child_fail
    mov dword ptr [clone_seen], 0x54485244
    mov eax, 1              /* child thread exit, not process exit */
    xor ebx, ebx
    int 0x80
    jmp fail

clone_child_fail:
    mov dword ptr [clone_seen], 0xBAD0BAD0
    mov eax, 1
    mov ebx, 1
    int 0x80
    jmp fail

after_clone:

    /* getcwd and basic access/fcntl metadata used by libc and npm. */
    mov eax, 183             /* getcwd */
    mov ebx, offset cwd_buffer
    mov ecx, 128
    int 0x80
    cmp eax, 2               /* boot cwd is / */
    jne fail
    cmp byte ptr [cwd_buffer], '/'
    jne fail

    mov eax, 33              /* access(/programs, R_OK) */
    mov ebx, offset programs_path
    mov ecx, 4
    int 0x80
    test eax, eax
    jne fail

    /* Linux ELF runtime probes commonly used during startup. */
    mov eax, 122             /* uname */
    mov ebx, offset utsname
    int 0x80
    test eax, eax
    jne fail
    cmp dword ptr [utsname], 0x756e694c /* "Linu" */
    jne fail

    mov eax, 265             /* clock_gettime(CLOCK_MONOTONIC, ...) */
    mov ebx, 1
    mov ecx, offset clock_value
    int 0x80
    test eax, eax
    jne fail

    mov eax, 355             /* getrandom(buf, 16, 0) */
    mov ebx, offset random_bytes
    mov ecx, 16
    xor edx, edx
    int 0x80
    cmp eax, 16
    jne fail

    mov eax, 162             /* nanosleep({0, 1ms}, NULL) */
    mov dword ptr [sleep_value], 0
    mov dword ptr [sleep_value + 4], 1000000
    mov ebx, offset sleep_value
    xor ecx, ecx
    int 0x80
    test eax, eax
    jne fail

    mov eax, 197             /* fstat64(stdout, ...) */
    mov ebx, 1
    mov ecx, offset stat_value
    int 0x80
    test eax, eax
    jne fail
    cmp dword ptr [stat_value + 16], 0x2000
    jne fail

    /* open(/programs, O_DIRECTORY), enumerate it, then close. */
    mov eax, 5
    mov ebx, offset programs_path
    mov ecx, 0x10000
    xor edx, edx
    int 0x80
    cmp eax, 0xfffff001
    jae fail
    mov edi, eax
    mov eax, 220             /* getdents64 */
    mov ebx, edi
    mov ecx, offset dirent_buffer
    mov edx, 4096
    int 0x80
    test eax, eax
    jle fail
    mov eax, 55              /* fcntl(dirfd, F_GETFD) */
    mov ebx, edi
    mov ecx, 1
    int 0x80
    test eax, eax
    jne fail
    mov eax, 6
    mov ebx, edi
    int 0x80
    test eax, eax
    jne fail

    /* symlink + readlink returns the exact target bytes, no trailing NUL. */
    mov eax, 83
    mov ebx, offset symlink_target
    mov ecx, offset symlink_path
    int 0x80
    test eax, eax
    jne fail
    mov eax, 85
    mov ebx, offset symlink_path
    mov ecx, offset symlink_buffer
    mov edx, symlink_target_end - symlink_target - 1
    int 0x80
    cmp eax, symlink_target_end - symlink_target - 1
    jne fail
    mov eax, 10              /* unlink test symlink */
    mov ebx, offset symlink_path
    int 0x80
    test eax, eax
    jne fail

    /* mmap2(NULL, 4096, PROT_READ|PROT_WRITE,
     *       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    xor ebx, ebx
    mov ecx, 4096
    mov edx, 3
    mov esi, 0x22
    mov edi, -1
    xor ebp, ebp
    mov eax, 192
    int 0x80
    cmp eax, 0xfffff001
    jae fail
    mov esi, eax
    mov dword ptr [esi], 0x12345678

    /* Read-only -> PROT_NONE reservation -> read/write commitment. */
    mov eax, 125
    mov ebx, esi
    mov ecx, 4096
    mov edx, 1
    int 0x80
    test eax, eax
    jne fail
    cmp dword ptr [esi], 0x12345678
    jne fail

    mov eax, 125
    mov ebx, esi
    mov ecx, 4096
    xor edx, edx
    int 0x80
    test eax, eax
    jne fail

    mov eax, 125
    mov ebx, esi
    mov ecx, 4096
    mov edx, 3
    int 0x80
    test eax, eax
    jne fail
    mov dword ptr [esi], 0x87654321
    cmp dword ptr [esi], 0x87654321
    jne fail

    /* munmap(addr, 4096) */
    mov eax, 91
    mov ebx, esi
    mov ecx, 4096
    int 0x80
    test eax, eax
    jne fail

    mov eax, 4              /* Linux i386 ABI: write(fd, buf, len) */
    mov ebx, 1
    mov ecx, offset message
    mov edx, message_end - message
    int 0x80

exit_ok:
    mov eax, 1              /* Linux i386 ABI: exit(0) */
    xor ebx, ebx
    int 0x80
1:
    jmp 1b

fail:
    mov eax, 1
    mov ebx, 1
    int 0x80
2:
    jmp 2b

.section .rodata
message:
    .ascii "ELF32 Linux ABI syscalls reached\n"
message_end:
child_path:
    .asciz "/programs/elf32_hello.elf"
child_argv0:
    .asciz "/programs/elf32_hello.elf"
child_argv1:
    .asciz "syscall-args"
child_env0:
    .asciz "TOS_USER_SPAWN=1"
exec_argv0:
    .asciz "/programs/elf32_hello.elf"
exec_argv1:
    .asciz "execve-args"
exec_env0:
    .asciz "TOS_EXECVE=1"
programs_path:
    .asciz "/programs"
symlink_target:
    .asciz "/programs/hello.t"
symlink_target_end:
symlink_path:
    .asciz "/home/.elf32-link-test"
.align 4
child_argv:
    .long child_argv0, child_argv1, 0
child_envp:
    .long child_env0, 0
exec_argv:
    .long exec_argv0, exec_argv1, 0
exec_envp:
    .long exec_env0, 0

.section .bss
.align 4
wait_status:
    .long 0
clone_tid:
    .long 0
clone_seen:
    .long 0
sleep_value:
    .space 8
clock_value:
    .long 0, 0
random_bytes:
    .space 16
stat_value:
    .space 96
dirent_buffer:
    .space 4096
utsname:
    .space 390
cwd_buffer:
    .space 128
symlink_buffer:
    .space 128
clone_stack:
    .space 4096

.section .data
.align 4
tls_desc:
    .long -1
    .long tls_value
    .long 0x000fffff
    .long 0x11                /* 32-bit segment, limit in pages */
tls_value:
    .long 0x746f5354
clone_tls_desc:
    .long -1
    .long clone_tls_value
    .long 0x000fffff
    .long 0x51
clone_tls_value:
    .long 0x746f5354
