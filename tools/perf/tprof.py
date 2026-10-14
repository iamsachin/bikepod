import serial,time,sys,re,subprocess
S=sys.argv[1]
s=serial.Serial('/dev/cu.usbmodem5B910646211',921600,timeout=0.2)
def cmd(c): s.write((c+'\n').encode()); time.sleep(0.3)
def collect(sec):
    end=time.time()+sec; buf=""
    while time.time()<end: buf+=s.read(8000).decode(errors='ignore')
    return [l for l in buf.splitlines() if l.startswith('TPROF')]
elf=[l for l in subprocess.run(['ls',S+'/build'],capture_output=True,text=True).stdout.split() if l.endswith('.ino.elf')][0]
syms={}
for l in subprocess.run(['/Users/sachin/Library/Arduino15/packages/esp32/tools/esp-x32/2302/bin/xtensa-esp32-elf-gcc-nm','-C',S+'/build/'+elf],capture_output=True,text=True).stdout.splitlines():
    p=l.split(' ',2)
    if len(p)==3 and p[0]: syms[int(p[0],16)]=p[2]
s.read(100000)
for c in sys.argv[2:]: cmd(c)
cmd("tprof"); time.sleep(2.5); collect(0.1)
for l in collect(4.2):
    a=int(re.search(r'0x([0-9a-f]+)',l).group(1),16); print(syms.get(a,hex(a)).ljust(34), l.split(':',1)[1])
cmd("tprof")
