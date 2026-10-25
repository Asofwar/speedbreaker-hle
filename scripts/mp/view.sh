#!/bin/sh
# usage: view.sh <out.png> [divisor]: the game window, reduced (small, to spare the quota)
geo=$(hyprctl clients -j | python3 -c "
import json,sys
for c in json.load(sys.stdin):
    if c.get('class')=='SpeedBreaker': print('%d,%d %dx%d'%(c['at'][0],c['at'][1],c['size'][0],c['size'][1]))")
grim -g "$geo" /tmp/ao/_view.png && python3 -c "
from PIL import Image
im=Image.open('/tmp/ao/_view.png'); d=int('${2:-4}'); im.resize((im.width//d,im.height//d)).save('$1')"
