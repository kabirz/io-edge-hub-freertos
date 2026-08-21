"""Sign fw.bin with imgtool (RSA-2048), convert to hex and merge with boot.

Inputs : build/boot.hex, build/fw.elf, tools/keys/root-rsa2048.pem
Outputs: build/fw.bin            (raw app image, vectors at offset 0)
         build/fw.signed.bin     (MCUboot image: 0x200 header + app + TLV)
         build/fw.signed.hex     (at slot0 0x08010000)
         build/full.hex          (boot + signed app, full-chip flash)
"""
import os
import shutil
import subprocess
import sys
import sysconfig

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 构建目录可覆盖 (Linux 用 build-linux, 避免与 Windows 的 CMake 缓存冲突)
BUILD = os.environ.get('BUILD_DIR') or os.path.join(ROOT, 'build')
KEY = os.path.join(ROOT, 'tools', 'keys', 'root-rsa2048.pem')

SLOT0 = 0x08010000
HDR = 0x200
SLOT_SIZE = 0x70000
MAX_SECTORS = 120


def find_objcopy():
    """跨平台定位交叉 objcopy: 环境变量 -> PATH -> CMake 缓存编译器同
    目录 -> 已知 SDK 路径 (Windows 开发机)。"""
    env = os.environ.get('CROSS_OBJCOPY')
    if env and os.path.exists(env):
        return env
    for name in ('arm-zephyr-eabi-objcopy', 'arm-none-eabi-objcopy'):
        p = shutil.which(name)
        if p:
            return p
    try:
        for line in open(os.path.join(BUILD, 'CMakeCache.txt'),
                         encoding='utf-8', errors='replace'):
            if line.startswith('CMAKE_C_COMPILER:FILEPATH='):
                cc = line.split('=', 1)[1].strip()
                d = os.path.dirname(cc)
                base = os.path.basename(cc)
                if base.endswith('-gcc'):
                    cand = os.path.join(d, base[:-4] + '-objcopy')
                    if os.path.exists(cand):
                        return cand
                for name in ('arm-zephyr-eabi-objcopy',
                             'arm-none-eabi-objcopy'):
                    cand = os.path.join(d, name + ('.exe'
                                       if os.name == 'nt' else ''))
                    if os.path.exists(cand):
                        return cand
                break
    except OSError:
        pass
    sdk = os.path.expanduser(
        r'~/zephyr-sdk-0.17.0/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy')
    if os.path.exists(sdk):
        return sdk
    win = (r'C:\Users\jxwaz\zephyr-sdk-0.17.0\arm-zephyr-eabi\bin'
           r'\arm-zephyr-eabi-objcopy.exe')
    if os.path.exists(win):
        return win
    sys.exit('cross objcopy not found (set CROSS_OBJCOPY)')


OBJCOPY = find_objcopy()


def imgtool():
    """imgtool 可执行 (Windows: scripts/imgtool.exe; Linux: PATH 或
    python -m imgtool)。"""
    p = shutil.which('imgtool')
    if p:
        return [p]
    for name in ('imgtool.exe', 'imgtool', 'imgtool.py'):
        cand = os.path.join(sysconfig.get_path('scripts'), name)
        if os.path.exists(cand):
            return [cand]
    return [sys.executable, '-m', 'imgtool']


def run(cmd):
    print('+', ' '.join(cmd))
    r = subprocess.run(cmd, capture_output=True)
    out = (r.stdout + r.stderr).decode('utf-8', errors='replace')
    if out.strip():
        print(out)
    if r.returncode != 0:
        sys.exit(1)


def main():
    # app 版本号 (VERSION 文件)
    ver = {}
    with open(os.path.join(ROOT, 'VERSION')) as f:
        for line in f:
            if '=' in line:
                k, v = line.split('=')
                ver[k.strip()] = v.strip()
    version = '%s.%s.%s' % (ver['VERSION_MAJOR'], ver['VERSION_MINOR'],
                            ver['PATCHLEVEL'])

    fw_elf = os.path.join(BUILD, 'fw.elf')
    boot_elf = os.path.join(BUILD, 'boot.elf')
    fw_bin = os.path.join(BUILD, 'fw.bin')
    signed_bin = os.path.join(BUILD, 'fw.signed.bin')
    full_bin = os.path.join(BUILD, 'full.bin')
    full_hex = os.path.join(BUILD, 'full.hex')

    run([OBJCOPY, '-O', 'binary', fw_elf, fw_bin])

    run(imgtool() + [
        'sign', fw_bin, signed_bin,
        '--key', KEY,
        '--header-size', str(HDR), '--pad-header',
        '--align', '8', '--version', version,
        '--slot-size', hex(SLOT_SIZE), '--max-sectors', str(MAX_SECTORS),
        '--erased-val', '0xff',
    ])

    # full 镜像 = boot.bin 填充到 slot0 起点 + 签名 app (bin 层拼接,
    # 一次 objcopy 出 hex, 避免 intelhex 合并)
    boot_bin = os.path.join(BUILD, 'boot.bin')
    full_bin = os.path.join(BUILD, 'full.bin')
    run([OBJCOPY, '-O', 'binary', boot_elf, boot_bin])
    with open(boot_bin, 'rb') as f:
        boot = f.read()
    with open(signed_bin, 'rb') as f:
        app = f.read()
    with open(full_bin, 'wb') as f:
        f.write(boot)
        f.write(b'\xff' * (SLOT0 - 0x08000000 - len(boot)))
        f.write(app)
    run([OBJCOPY, '-I', 'binary', '-O', 'ihex',
         '--change-addresses=%d' % 0x08000000, full_bin, full_hex])
    print('full.bin: %d bytes (boot %d + app %d)' %
          (len(boot) + (SLOT0 - 0x08000000 - len(boot)) + len(app),
           len(boot), len(app)))


if __name__ == '__main__':
    main()
