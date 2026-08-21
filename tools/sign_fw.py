"""Sign fw.bin with imgtool (RSA-2048), convert to hex and merge with boot.

Inputs : build/boot.hex, build/fw.elf, tools/keys/root-rsa2048.pem
Outputs: build/fw.bin            (raw app image, vectors at offset 0)
         build/fw.signed.bin     (MCUboot image: 0x200 header + app + TLV)
         build/fw.signed.hex     (at slot0 0x08010000)
         build/full.hex          (boot + signed app, full-chip flash)
"""
import os
import subprocess
import sys
import sysconfig

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build')
SDK_BIN = r'C:\Users\jxwaz\zephyr-sdk-0.17.0\arm-zephyr-eabi\bin'
OBJCOPY = os.path.join(SDK_BIN, 'arm-zephyr-eabi-objcopy.exe')
KEY = os.path.join(ROOT, 'tools', 'keys', 'root-rsa2048.pem')

SLOT0 = 0x08010000
HDR = 0x200
SLOT_SIZE = 0x70000
MAX_SECTORS = 120


def imgtool():
    exe = os.path.join(sysconfig.get_path('scripts'), 'imgtool.exe')
    if not os.path.exists(exe):
        exe = os.path.join(sysconfig.get_path('scripts'), 'imgtool')
    return [exe]


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
