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
adb shell su -c "insmod /data/local/tmp/modcapture.ko"
# 内核若抱怨 vermagic/KMI，改用 KernelSU 的加载器：
adb shell su -c "ksud insmod /data/local/tmp/modcapture.ko"
```

本模块**只 import 导出符号**，一个未导出符号都没有（`load_module` 是靠 `register_kprobe()`
按名字找到的，不产生链接期依赖），所以**不需要** ksud 那套符号改写，普通 `insmod` 就够。
唯一需要留意的是 `filp_open/kernel_write/override_creds/revert_creds` 走
`ANDROID_GKI_VFS_EXPORT_ONLY` 命名空间，源码末尾已经 `MODULE_IMPORT_NS` 了那个长字符串
（CI 会在产物里断言 `import_ns` 存在，缺了就直接失败）。

vermagic 由 DDK 那棵内核树给出：`5.15.202-android13-5.15.202_r00-dirty SMP preempt
mod_unload modversions aarch64`，设备内核是 `5.15.180-android13-8`，版本号对不上。
但 DDK 的 `Module.symvers` 不带 CRC，产物的 `__versions` 段**存在但为空**，而内核的
`same_magic()` 只要发现模块有 `__versions` 段就**跳过第一个空格之前的内核版本**、只比后面
那串（`SMP preempt mod_unload modversions aarch64`）。所以这个版本差通常不影响加载；
真过不去就用 `ksud insmod`。

卸载：

```sh
adb shell su -c "rmmod modcapture"
```

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

- **只有加载顺序在后面的模块**会被抓（定义如此）。改 `enabled=0/1` 只能关掉以后的捕获，不能补回之前的。
- **超大模块**（> `slot_size_mb`）计数后跳过，`rmmod` 时会打印 `oversized=N`。
- **`rmmod` 会解除探针**，模块卸载后不再捕获。
- 本模块**不隐藏自己**：`/proc/modules`、`lsmod` 里都看得到 `modcapture`。
- `load_module()` 在 `CONFIG_LTO_CLANG_FULL` 下有可能被内联掉。本机
  （`5.15.180-android13-8`）实测 `/proc/kallsyms` 里有它；如果哪个内核没有，
  `register_kprobe()` 会在 `module_init` 里失败并**明确报错拒绝加载**，而不是装成一个没用的模块。

## 设备上验证

`tools/verify-modcapture.sh`：

```sh
adb push dist/modcapture.ko-android13-5.15/modcapture.ko /data/local/tmp/modcapture.ko
adb push tools/verify-modcapture.sh /data/local/tmp/
adb shell su -c "sh /data/local/tmp/verify-modcapture.sh"
```

它会：确认探针装上 → 实际触发一次加载 → 找到新生成的 `*.ko` → **和源文件逐字节比对 md5**
→ 检查文件名符合时间戳格式 → 检查 `modcapture.log` → 扫 dmesg 里有没有
`BUG:/WARNING:/CFI failure/Oops`，最后卸干净。
