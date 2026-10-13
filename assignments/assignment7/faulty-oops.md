# Assignment 7: kernel oops from /dev/faulty

## How it was produced

- Image: the Buildroot image from the assignment 5 repository (Linux 6.18.7, aarch64, QEMU `virt` machine,
  `qemu-system-aarch64 -M virt -cpu cortex-a53`).
- The ldd3 `faulty` module is loaded at boot by the `S98lddmodules` init script, which creates `/dev/faulty`
  (character device, major 247). `lsmod` showed `hello`, `faulty` and `scull` loaded.
- `/proc/sys/kernel/panic_on_oops` was `0`, so the kernel kills the offending process and keeps running.
- Trigger, run over ssh as root in the guest shell:

```sh
echo "hello_world" > /dev/faulty
```

The remote shell died and the ssh client exited with status 255. The oops below was read back afterwards with
`dmesg`. The same block also appeared on the QEMU serial console, identical apart from carriage returns.

## Oops output

```text
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000044
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x04: level 0 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000044, ISS2 = 0x00000000
  CM = 0, WnR = 1, TnD = 0, TagAccess = 0
  GCS = 0, Overlay = 0, DirtyBit = 0, Xs = 0
user pgtable: 4k pages, 48-bit VAs, pgdp=0000000041c9e000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000
Internal error: Oops: 0000000096000044 [#1]  SMP
Modules linked in: hello(O) faulty(O) scull(O)
CPU: 0 UID: 0 PID: 171 Comm: sh Tainted: G           O        6.18.7 #1 NONE 
Tainted: [O]=OOT_MODULE
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x4/0x10 [faulty]
lr : vfs_write+0xb8/0x3c0
sp : ffff800080f63d60
x29: ffff800080f63de0 x28: ffff000001a54ec0 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 0000000040000000 x22: 0000ffff8529c0a8 x21: 00000000ffffffff
x20: ffff000001c7b0c0 x19: ffff000001c7b0c0 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000001 x7 : ffff000001c7b0c0 x6 : ffff800078d4a080
x5 : 000000000000000c x4 : 0000aaaae4f6f480 x3 : ffff800080f63e00
x2 : 000000000000000c x1 : 0000aaaae4f6f480 x0 : 0000000000000000
Call trace:
 faulty_write+0x4/0x10 [faulty] (P)
 ksys_write+0x64/0x100
 __arm64_sys_write+0x18/0x30
 invoke_syscall.constprop.0+0x40/0xf0
 el0_svc_common.constprop.0+0x38/0xe0
 do_el0_svc+0x1c/0x30
 el0_svc+0x34/0xf0
 el0t_64_sync_handler+0xa0/0xf0
 el0t_64_sync+0x198/0x19c
Code: 00000000 00000000 00000000 d2800000 (b900001f) 
---[ end trace 0000000000000000 ]---
```

## What the important parts mean

- **`Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000`**: kernel code
  accessed address 0, which is not mapped. In this oops the access is a write through a NULL pointer.
- **`Mem abort info` / `Data abort info`**: the CPU's description of the fault, decoded from the ESR register.
  - `ESR = 0x0000000096000044` is the raw exception syndrome.
  - `EC = 0x25: DABT (current EL)` is a data abort taken at the current exception level, i.e. in kernel mode
    (the faulting access was made by kernel code, not by user space).
  - `WnR = 1` means the faulting access was a write, not a read.
  - `FSC = 0x04: level 0 translation fault` means no page-table entry maps the address
    (`pgd=0000000000000000` in the line `[0000000000000000] pgd=..., p4d=...`).
- **`Internal error: Oops: 0000000096000044 [#1]  SMP`**: the kernel's oops report. The hex value is the ESR again,
  `[#1]` says this is the first oops since boot, and `SMP` is a kernel built with SMP support (QEMU ran one CPU).
- **`Modules linked in: hello(O) faulty(O) scull(O)`**: the modules loaded at the time. `(O)` marks an out-of-tree
  module, which taints the kernel (see the `Tainted` lines).
- **`CPU: 0 UID: 0 PID: 171 Comm: sh Tainted: G           O        6.18.7 #1 NONE`**: the oops happened on CPU 0 in
  process 171, whose command name is `sh` (the shell that ran the `echo` redirection) and which runs as UID 0.
  In `Tainted: G O`, `G` means no proprietary module is loaded and `O` means at least one out-of-tree module was
  loaded (here all three ldd3 modules; see `Tainted: [O]=OOT_MODULE`). `6.18.7` is the kernel version and `#1` its
  build number.
- **`Hardware name: linux,dummy-virt (DT)`**: the machine name from the device tree that QEMU's `virt` board
  generated.
- **`pc : faulty_write+0x4/0x10 [faulty]`**: `pc` is the program counter, the address of the instruction that
  faulted. It is at offset 0x4 into the function `faulty_write`, which the kernel reports as 0x10 bytes long, in the
  module `faulty`.
- **`lr : vfs_write+0xb8/0x3c0`**: `lr` (x30) is the link register, the return address of the current function.
  `faulty_write` was called by `vfs_write` (the VFS layer calls the driver's `.write` method), so the return
  address lies 0xb8 bytes into `vfs_write`.
- **`sp : ffff800080f63d60`** and the register dump: `sp` is the kernel stack pointer, `x29` the frame pointer,
  `x0` to `x28` the general registers. `x0 = 0000000000000000` is the NULL pointer being written through.
  `x1 = 0000aaaae4f6f480` and `x2 = 000000000000000c` are the user buffer and byte count of the `write` call
  (12 bytes, which is `hello_world` plus the newline).
- **Call trace**: read from the bottom up, it shows how execution got into the module. `el0t_64_sync` is the entry
  for a synchronous exception from user space (EL0), `el0t_64_sync_handler`, `el0_svc` and `do_el0_svc` dispatch
  the system call, `el0_svc_common` and `invoke_syscall` run the syscall table entry `__arm64_sys_write`, which
  calls `ksys_write`, which calls `vfs_write`, which calls `faulty_write` at the top. `(P)` marks the entry taken
  from the saved `pc`. `vfs_write` has no line of its own: `faulty_write` is a leaf function that never pushes a
  frame record, so the frame pointer `x29` still points at `vfs_write`'s record, and the unwinder's first step
  lands on `vfs_write`'s own return address in `ksys_write`. `vfs_write` is still visible in the `lr` line above.
- **`Code: 00000000 00000000 00000000 d2800000 (b900001f)`**: the instruction words around `pc`. The word in
  parentheses, `b900001f`, is the faulting instruction, `str wzr, [x0]` (store a 32-bit zero to the address in
  `x0`). The word before it, `d2800000`, is `mov x0, #0`. The three `00000000` words come before the function.
- **`---[ end trace 0000000000000000 ]---`**: marks the end of the oops report. The trace id is zero here.

## Finding the faulting line in faulty.c

The unstripped module comes from the Buildroot build directory. The tool prefix is
`aarch64-buildroot-linux-gnu-` from `buildroot/output/host/bin/`.

```sh
K=buildroot/output/build/ldd-acae6be1e93b2af5052dfe0415bca480be7754d6/misc-modules/faulty.ko
P=buildroot/output/host/bin/aarch64-buildroot-linux-gnu-
${P}objdump -d --disassemble=faulty_write $K
${P}objdump -S --disassemble=faulty_write $K
${P}readelf -S $K | grep -i debug
${P}nm -S $K | grep faulty_write
${P}addr2line -f -e $K 0x4
```

`objdump -d` output (the `-S` variant printed the same, with no source lines):

```text
0000000000000000 <faulty_write>:
   0:	d2800000 	mov	x0, #0x0                   	// #0
   4:	b900001f 	str	wzr, [x0]
   8:	d65f03c0 	ret
```

- The module has no debug info: `readelf -S ... | grep -i debug` printed nothing. Hence `objdump -S` shows no
  source and `addr2line -f -e faulty.ko 0x4` only answers `faulty_write` / `??:?`. The function name is found, but
  there is no file or line.
- `faulty_write` starts at 0x0 in `.text` of the `.ko`, so `pc` offset `+0x4` is address 0x4: the instruction
  `str wzr, [x0]`. It is the store of 0 through `x0`, and the instruction just before it set `x0` to 0. This is the
  same word `b900001f` shown in parentheses in the `Code:` line.
- The disassembly lists three instructions (0xc bytes, `nm -S` gives size 0xc), while the oops prints a function
  size of 0x10, presumably the kernel's symbol size including alignment padding up to the next symbol.
  The offset 0x4 is the part that matters here.
- With no debug info, the mapping to C is done by reading the source. `faulty_write` in `misc-modules/faulty.c`
  (lines 49 to 55 of the ldd3 repository at `HEAD`) is:

```c
ssize_t faulty_write (struct file *filp, const char __user *buf, size_t count,
		loff_t *pos)
{
	/* make a simple fault by dereferencing a NULL pointer */
	*(int *)0 = 0;
	return 0;
}
```

The function consists of `mov x0, #0` plus `str wzr, [x0]` (the body of line 53), then `ret` (line 54). The faulting
store at `faulty_write+0x4` is therefore line 53, `*(int *)0 = 0;`, in `faulty_write`. It writes an `int` zero to
address 0, which gives the translation fault and the NULL pointer dereference seen above.
