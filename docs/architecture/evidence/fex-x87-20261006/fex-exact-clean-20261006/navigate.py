"""Known 2560x1600 game coordinates; inspect each resulting screenshot."""
import argparse
import time
import automate as a
p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('stage', choices=['new', 'map', 'confirm', 'ninja', 'baby', 'sun', 'salon'])
p.add_argument('--settle', type=float, default=10)
opt = p.parse_args()
points = dict(new=(850,970), map=(1645,1045), confirm=(2130,1465),
    ninja=(1818,455), baby=(1975,610), sun=(2190,455), salon=(2190,675))
x,y = points[opt.stage]
a.shell(opt.name + '-input', f'uinput -T -c {x} {y} 60', policy='automation')
assert 0 <= opt.settle <= 20
time.sleep(opt.settle)
a.snapshot(opt.name)
