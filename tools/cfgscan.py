import re
import glob

s = set()
for p in glob.glob(r'deps\mcuboot\boot\bootutil\src\*.c') + \
         glob.glob(r'deps\mcuboot\boot\bootutil\src\*.h') + \
         glob.glob(r'deps\mcuboot\boot\bootutil\include\bootutil\*.h'):
    for m in re.finditer(r'MCUBOOT_[A-Z0-9_]+',
                         open(p, encoding='utf-8', errors='replace').read()):
        s.add(m.group())
out = '\n'.join(sorted(s))
open('tools_cfg.txt', 'w').write(out)
print(len(s))
