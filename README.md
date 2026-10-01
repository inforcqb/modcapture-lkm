# modcapture-lkm

**把本模块加载之后所有被加载的内核模块的原始镜像抓下来**，按 `[时间戳].ko` 落到 `/data/local/tmp`。

用途：设备上有人（或某个 vendor 服务、某个 root 工具）`insmod` 了一个 `.ko`，你想知道它到底是什么、
跟谁比对、是不是被 memfd 塞进去的 —— 只要 `modcapture` 先加载，之后每一次加载都会在
`/data/local/tmp/` 留下一个**与原文件逐字节相同**的副本。

## 原理

挂在 **`load_module()`** 上（`kernel/module.c`）的 kprobe，入口处 `x0` 就是
`struct load_info *info`。此时：

| 加载路径 | 内核里那份原始文件是怎么来的 |
|---|---|
| `init_module(2)` | `copy_module_from_user()` → `__vmalloc(len)` + `copy_from_user` |
| `finit_module(2)` | `kernel_read_file_from_fd(..., READING_MODULE)`，`info.len` = 文件大小 |

两条路都汇到 `load_module(&info, uargs, flags)`，`info->hdr` 指向**原文件的逐字节副本**，`info->len` 是它的大小。

选这里的理由：

1. **只有这里还有原始文件**。`load_module()` 进来之后马上就会**原地改写 section header**
   （`rewrite_section_headers()`），退出时再 `free_copy()` 把这份副本释放掉。挂 `do_init_module()`
   只能拿到重定位后的内存映像，那不是 `.ko`。
2. **memfd 加载也照样抓得到**。Android 上常见做法是用 `memfd_create` 把 `.ko` 藏起来再 `finit_module`，
   fd 路径根本追不到；但等我们的探针命中时，内核已经把内容拷进自己的内存了。
3. **校验之前就抓**。vermagic 不对、签名不过、符号缺失 —— 加载会失败，但**东西已经被 dump 下来了**。
   通常真正想知道的正是"它到底试图加载了什么"。

## 为什么文件不是在探针里写的

kprobe 的 pre-handler 处在**关抢占、不能睡眠**的上下文里，开文件/写文件都不允许。所以探针只做：

```
memcpy 到预分配的 staging slot  →  queue_work  →  返回
```

文件 I/O 全部交给 workqueue。所有 staging 缓冲区在 `module_init` 里一次性 `vmalloc` 好，
探针本身**不做任何分配**。`module_mutex` 保证模块加载是串行的，但写盘不是，所以默认给了 2 个 slot。

**credential**：写盘用 `override_creds()` 换成"发起加载的那个进程"的凭据。kworker 跑在内核域，
不一定有权在 `/data/local/tmp` 建文件；而发起加载的进程既然能读那个 `.ko`、又通常是 root/su，
它本来就有这个权限。这样"写成功"和"失败"都对应真实语义，不会被 SELinux 域差异搅浑。

## 构建

CI（`.github/workflows/build-ddk.yml`）用 Android DDK 镜像 `ghcr.io/ylarod/ddk-min:<variant>-20260828`
交叉编译，每个 GKI 变体一份产物：

| 变体 | 用于 |
|---|---|
| `modcapture.ko-android13-5.15` | Android 13 的 5.15 GKI（本机 PJA110 / `5.15.180-android13-8` 就是这支） |
| `modcapture.ko-android14-5.15` | Android 14 的 5.15 GKI |

> 只有 5.15。`struct load_info` 的字段布局是按 5.15 镜像的，源码里有编译期版本门禁
> （非 5.15 直接 `#error`），CI 里每个变体还会去**目标内核树**里核对
> `kernel/module-internal.h` 的字段顺序和 `load_module()` 的原型，对不上就拒绝出产物。

本地编译（需要有 DDK 或已 `modules_prepare` 的内核树）：

```sh
make -C $KDIR M=$PWD/kernel ARCH=arm64 CC=clang LLVM=1 LLVM_IAS=1 modules
```

## 加载

```sh
adb push dist/modcapture.ko-android13-5.15/modcapture.ko /data/local/tmp/modcapture.ko
adb shell su -c "ksud insmod /data/local/tmp/modcapture.ko"
```

**必须用 `ksud insmod`**（和 `susfs_guard_lkm` 一样）。真机实测，普通 `insmod` 会在
**符号解析**阶段失败：

```
modcapture: Unknown symbol filp_open (err -2)
modcapture: Unknown symbol kernel_write (err -2)
insmod: failed to load /data/local/tmp/modcapture.ko: No such file or directory
```

`err -2` 是 `-ENOENT`，即**这台设备的内核根本没把这两个符号导出给模块**。注意这跟
"DDK 源码树里 `EXPORT_SYMBOL_NS(filp_open, ANDROID_GKI_VFS_EXPORT_ONLY)` 存在"并不矛盾：
CI 只能核对**编译所用的那棵源码树**（这一步仍然值得做，它挡的是"源码里就没导出"），
但**设备上装的那颗内核导出什么，只有设备自己知道**。`ksud insmod` 绕开的正是这一层：
它在 `init_module(2)` 之前把每个未定义符号的 `st_value` 直接填成 kallsyms 里的运行时地址并标成绝对符号，
所以内核的导出表查找根本不会发生。

`filp_open/kernel_write/override_creds/revert_creds` 走 `ANDROID_GKI_VFS_EXPORT_ONLY` 命名空间，
源码末尾已经 `MODULE_IMPORT_NS` 了那个长字符串，CI 也会在产物里断言 `import_ns` 存在。

vermagic 由 DDK 那棵内核树给出：`5.15.202-android13-5.15.202_r00-dirty SMP preempt
mod_unload modversions aarch64`，设备内核是 `5.15.180-android13-8`。但 DDK 的
`Module.symvers` 不带 CRC，产物的 `__versions` 段**存在但为 0 字节**，而内核的
`same_magic()` 只要发现模块有 `__versions` 段就**跳过第一个空格之前的内核版本**、只比后面
那串（`SMP preempt mod_unload modversions aarch64`）—— 实测这一关是过得去的，
真正卡住你的是上面的符号导出。

卸载：

```sh
adb shell su -c "rmmod modcapture"
```

## 抓到的到底是什么

**是"内核被要求加载的那份镜像"**，不是"磁盘上那个文件"。两者在正常情况下相同，但有个例外必须知道：

| 加载方式 | 内核收到的东西 | dump 与磁盘文件 |
|---|---|---|
| `insmod` / `modprobe`（`finit_module(2)` 直接读文件） | 原文件的逐字节副本 | **完全一致**（真机 md5 实测相同） |
| KernelSU 的 `ksud insmod` | ksud **在系统调用之前**改写过符号表的镜像 | 大小相同、ELF 完整，但 `st_value` 已被填成内核地址 |

第二条不是缺陷，是物理事实：**ksud 改完才交给内核，内核里从来就不存在那份"原文件"**，
所以任何在内核侧挂的钩子都拿不到未改写的版本。换个角度说，ksud 路径 dump 出来的东西
反而多带一个信息 —— 每个符号被绑到了哪个内核地址。

真机实测（modcapture.ko 自捕获，309656 字节）：两条路各出一份，`insmod` 那份 md5 与源文件相同；
`ksud` 那份共 **389 字节 / 40 段**不同，全部落在 `.symtab` 区间（`0x48d80`–`0x4a160`），
其余 section 一个字节都没动，`modinfo` 照常读得出 `name=modcapture`。

## 用法

```
# 加载之后随便 insmod 点什么（失败也行，见上文第 3 点）
adb shell su -c "insmod /data/local/tmp/modcapture.ko"     # 再 insmod 一次自己：EEXIST，但仍会被抓一次
adb shell su -c "ls -l /data/local/tmp/*.ko"
adb shell su -c "cat /data/local/tmp/modcapture.log"
```

产物：

```
/data/local/tmp/20260930-142530-123456.ko      # 被抓到的模块原始镜像
/data/local/tmp/modcapture.log                 # 一行一次捕获：时间/pid/comm/大小/模块名/文件名
```

`modcapture.log` 里那一行是**必要的**：文件名按需求就是纯时间戳，没有它就只能事后对每个文件跑
`modinfo` 才知道谁是谁。模块名是从我们自己那份私有副本的 `.modinfo` 里解出来的（内核此刻还没校验过
这个 buffer，所以解析里每一处都做了边界检查）。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `dump_dir=` | `/data/local/tmp` | 落盘目录 |
| `enabled=` | `1` | 运行时开关捕获（探针留着，只是直接返回） |
| `index=` | `1` | 是否写 `modcapture.log` |
| `slots=` | `2` | staging slot 数量（在途 dump 上限），1–16 |
| `slot_size_mb=` | `16` | 单个 slot 大小，1–256；比它大的模块**计数后跳过**，不截断 |

内存占用 = `slots × slot_size_mb`（默认 32 MiB vmalloc）。

```sh
insmod modcapture.ko slot_size_mb=64 slots=4 dump_dir=/data/local/tmp/ko
```

## 已知边界

- **本模块自己也得用 `ksud insmod` 加载**（原因见上文），因为它要写文件、而设备内核不导出 `filp_open`。
- **只有加载顺序在后面的模块**会被抓（定义如此）。改 `enabled=0/1` 只能关掉以后的捕获，不能补回之前的。
- **超大模块**（> `slot_size_mb`）计数后跳过，`rmmod` 时会打印 `oversized=N`。
- **`rmmod` 会解除探针**，模块卸载后不再捕获。
- 本模块**不隐藏自己**：`/proc/modules`、`lsmod` 里都看得到 `modcapture`。
- `load_module()` 在 `CONFIG_LTO_CLANG_FULL` 下有可能被内联掉。本机
  （`5.15.180-android13-8`）实测 `/proc/kallsyms` 里有它（同树上 `copy_module_from_user`、
  `module_sig_check`、`elf_validity_check`、`setup_load_info`、`rewrite_section_headers`、
  `layout_and_allocate` **全被内联掉了**，所以落点只能选 `load_module`）；
  如果哪个内核没有它，`register_kprobe()` 会在 `module_init` 里失败并**明确报错拒绝加载**，
  而不是装成一个没用的模块。

## 设备上验证

`tools/verify-modcapture.sh`：

```sh
adb push dist/modcapture.ko-android13-5.15/modcapture.ko /data/local/tmp/modcapture.ko
adb push tools/verify-modcapture.sh /data/local/tmp/
adb shell su -c "sh /data/local/tmp/verify-modcapture.sh"
```

它会：确认探针装上 → 实际触发两次加载（`insmod` / `ksud insmod`，两条路断言不同）→
找到新生成的 `*.ko` → **`insmod` 那份与源文件逐字节比对**、`ksud` 那份核对大小与可读性 →
检查文件名符合时间戳格式 → 检查 `modcapture.log` → 扫 dmesg 里有没有
`BUG:/WARNING:/CFI failure/Oops`，最后卸干净。

真机结果（PJA110 / `5.15.180-android13-8-o-01179` / KernelSU `u:r:ksu:s0`）：
`14 passed, 0 failed`（exit 0），`rmmod` 汇总 `captured=2 dropped=0 oversized=0 write_fail=0`，
dmesg 无 BUG/WARNING/CFI failure/Oops。

另外单独做了一次交叉验证：把 **android14-5.15 那份产物**（313296 字节，内容与源文件不同）
推上设备再触发一次加载。两次加载**都失败**（内核里已有一个同名模块，`-EEXIST`），但**两份 dump 都出来了** ——
这正是"捕获发生在校验之前"的实证：

```
99da46054b82624ffaa04ae7080ad1f3  other-a14.ko                       (源文件 313296 B)
99da46054b82624ffaa04ae7080ad1f3  20261001-140356-588988.ko          insmod 路径：完全一致
73c5a9a5dc2d4226055fe363ff634d39  20261001-140357-776147.ko          ksud 路径：389 字节不同
```
