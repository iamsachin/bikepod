# Replays a recorded pod feed (podfeed.log: "t<TAB>line") into the pod over USB, at the recorded
# pace, while saving everything the pod prints. Usage: replay.py feed.log out.log [extra serial cmds...]
import serial,time,sys,threading
feed,outp=sys.argv[1],sys.argv[2]
s=serial.Serial('/dev/cu.usbmodem5B910646211',921600,timeout=0.1)
lines=[l.rstrip('\n').split('\t',1) for l in open(feed) if '\t' in l]
lines=[(float(t),x) for t,x in lines if x]
out=open(outp,'w'); stop=False
def reader():
    buf=""
    while not stop:
        buf+=s.read(8000).decode(errors='ignore')
        while "\n" in buf:
            l,buf=buf.split("\n",1); out.write("%.2f %s\n"%(time.time()-t0,l.rstrip())); out.flush()
t0=time.time(); th=threading.Thread(target=reader,daemon=True); th.start()
for c in sys.argv[3:]: s.write((c+'\n').encode()); time.sleep(0.2)
t0=time.time()
for t,x in lines:
    d=t-(time.time()-t0)
    if d>0: time.sleep(d)
    s.write(("msg "+x+"\n").encode()); s.flush()
    time.sleep(0.012)        # about Bluetooth's pace for a burst of map lines; faster overflows the pod's USB input
time.sleep(2); stop=True; th.join()
print("replayed",len(lines),"lines in %.0f s"%(time.time()-t0))
