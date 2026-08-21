"""Regenerate the MCUboot signing keypair.

  python tools/gen_keys.py            # default tools/keys/root-rsa2048.pem

Outputs the RSA-2048 PEM (NEVER commit) and refreshes the public-key
array embedded in src/boot/mcuboot/keys.c (public, committed).
After regenerating, boot + app must both be reflashed via SWD.
"""
import os
import subprocess
import sys
import sysconfig

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def imgtool():
    exe = os.path.join(sysconfig.get_path('scripts'), 'imgtool.exe')
    if not os.path.exists(exe):
        exe = os.path.join(sysconfig.get_path('scripts'), 'imgtool')
    return exe


def main():
    pem = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'tools', 'keys', 'root-rsa2048.pem')
    subprocess.check_call([imgtool(), 'keygen', '-k', pem, '-t', 'rsa-2048'])
    out = subprocess.check_output([imgtool(), 'getpub', '-k', pem, '-o',
                                   'CON'])
    print(out.decode())
    print('pem:', pem)
    print('update src/boot/mcuboot/keys.c with the array above')


if __name__ == '__main__':
    main()
