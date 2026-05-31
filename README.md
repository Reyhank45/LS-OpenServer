# LS-OpenServer

A custom, hyper-minimalist, kernel-only system built on top of the production-grade FreeBSD kernel. This repository strips away the traditional Unix userland completely, replacing it with a custom, high-performance **pure C binary** acting directly as `PID 1` (`/System/sbin/init`).

The entire userland architecture is isolated within a modern, clean `/System/` hierarchy, bypassing legacy directory clutter while maintaining direct, raw communication with the underlying kernel subsystems.

---

## 🚀 Architectural Overview

- **Kernel:** Tailored, ultra-lean FreeBSD kernel footprint (~11MB).
- **Userland:** 0% legacy FreeBSD software. 100% custom C codebase.
- **Initialization (PID 1):** A statically linked, standalone C program (`init`) that establishes the initial execution space, manages kernel events, and boots directly into `lssh` (LS-OpenServer Shell).
- **Filesystem Tree:** Organized entirely inside a custom layout:
  ```text
  /
  └── System/
      ├── boot/
      │   ├── loader.conf      # Bootloader tunables
      │   └── kernel/
      │       ├── kernel       # Minimal FreeBSD kernel
      │       └── mfsroot.iso  # Preloaded memory disk userland image
      └── sbin/
          └── init             # Custom pure C init binary / Shell (lssh)

---

## OS Editions

- **Server Edition:**

A lightweight server-focused OS with **QEMU/KVM** support built-in. Optimized for virtualization, Fast deployment,and remote management. And new Specialized feature called "Production Mode" that builds a minimal, optimized image with specific configuration for specific hardware with less bloat and overhead.

- **Router Edition:**

A lightweight router-focused OS with **QEMU/KVM** support built-in. With special ability for management via MAC address or IP less management. 

## 🔨 Cross-Compilation Toolchain (Linux Host)

Since the target environment uses the native FreeBSD system call ABI, compiling directly with a standard Linux toolchain will cause a kernel panic (`ELF binary type "3" not known / Exec format error`).

We leverage standard LLVM/Clang on a Linux host (e.g., Fedora) as a bare-metal cross-compiler by stripping the host libraries and injecting native FreeBSD ABI branding notes.

### Compilation Command

Run the following command from your Linux host to cross-compile the initialization binary:

```bash
clang -target x86_64-unknown-freebsd14.0 \
      -nostdlib \
      -static \
      -Wl,--brandnote \
      -o init main.c

```

### Build Flags Explained:

* `-target x86_64-unknown-freebsd14.0`: Targets the machine architecture instructions directly to the FreeBSD system call convention.
* `-nostdlib`: Disables all host-specific Linux standard libraries (`glibc`) and runtime assembly files (`crt1.o`, etc.) to prevent environment pollution.
* `-static`: Binds all generated binary symbols natively inside a single executable.
* `-Wl,--brandnote`: Instructs the linker to inject an ELF note section explicitly tagging the output binary header as **FreeBSD Native (ABI Type 9)**. This guarantees immediate verification when probed by the kernel VFS layer.

---

## Low-Level System Calls (Bare-Metal C)

Because the binary executes in a zero-dependency environment without a standard user-space `libc`, traditional functions like `printf()` or `sleep()` are missing. Interaction with the terminal display, keyboard input, and network stack is handled via explicit assembly-wrapped kernel system calls:

```c
/* Raw assembly wrapper for FreeBSD 64-bit System Calls */
static inline long freebsd_syscall(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "movq %1, %%rax;\n"     /* System call number into RAX */
        "movq %2, %%rdi;\n"     /* Argument 1 into RDI */
        "movq %3, %%rsi;\n"     /* Argument 2 into RSI */
        "movq %4, %%rdx;\n"     /* Argument 3 into RDX */
        "syscall;\n"            /* Trigger kernel context switch */
        "movq %%rax, %0;\n"     /* Capture return register */
        : "=r" (ret)
        : "g" (num), "g" (arg1), "g" (arg2), "g" (arg3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

```

---

## Boot Configuration & Console Mapping

The system boots via the native FreeBSD UEFI loader (`loader.efi`) mapped inside your boot media. To instruct the kernel to find our non-standard `/System/` path and route keyboard inputs correctly, create the following configuration:

### `/boot/loader.conf` Configuration

```text
# Force the graphical console configuration layout
console="vidconsole"

# Mount the root directory directly from the preloaded RAM image
vfs.root.mountfrom="cd9660:/dev/md0"

# Redirect the kernel execution pointer to our custom init binary path
init_path="/System/sbin/init"

```

> **Note on Graphical Console Input:** If the bootloader defaults standard input/output channels exclusively to the serial terminal interface, your keyboard inputs on the GUI display may drop. Ensuring `console="vidconsole"` matches the underlying keyboard mapping engine (`hkbd`/`vt`) redirects `stdin` directly into your custom `lssh` prompt environment.

---

## ⚙️ Target ISO Deployment Flow

1. **Compile:** Build the bare-metal C binary on your Linux host using the branded Clang flag combination.
2. **Stage Layout:** Place the compiled `init` executable inside a temporary userland workspace directory at `/System/sbin/init`.
3. **Pack Media:** Format and pack the workspace layout into an ISO image (`mfsroot.iso`) using standard Linux filesystem utilities (`mkisofs` or `xorriso`).
4. **Boot Asset:** Place the generated image alongside your custom kernel on your target ESP/Boot partition partition layout, configure `loader.conf`, and execute.

## 🔐 License Summary

- **Userland (Source Code):** Licensed under **GPLv3**. The complete source code for `init`, `lssh`, and all utilities is open source and available in this repository.
- **Kernel (FreeBSD):** Licensed under **BSD 2-Clause**. The underlying kernel binary is derived from the FreeBSD Project, whose licensing terms are documented in `LICENSE-FREEBSD`.
