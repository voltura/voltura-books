"""Exercise actual Rich Edit page fitting, DPI mapping, and large picture goals."""
import io
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path
from PIL import Image

with tempfile.TemporaryDirectory(prefix="books-rtf-fit-") as folder:
    encoded=io.BytesIO()
    Image.new("RGB",(120,80),(0,128,128)).save(encoded,format="PNG")
    document=Path(folder)/"fit.rtf"
    document.write_text(r"{\rtf1\ansi\paperw11906\paperh16838\fs24 "
        +"Complete text across the page. "*15+r"\par {\pict\pngblip\picwgoal48000\pichgoal32000\picscalex20\picscaley20 "
        +encoded.getvalue().hex()+r"}\par End of page.}",encoding="ascii")
    process=subprocess.Popen([sys.argv[1],str(document)],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
    timeout=threading.Timer(20,process.kill)
    timeout.start()
    try:
        ends=[]
        for width,height,dpi in [(500,800,96),(500,800,192),(1920,1080,192),(3840,2160,192)]:
            process.stdin.write(struct.pack("5i",width,height,dpi,0,0));process.stdin.flush()
            response=process.stdout.read(20)
            assert len(response)==20,"Renderer exited before returning a page"
            w,h,end,length,anchor=struct.unpack("5i",response)
            assert (w,h,anchor)==(width,height,0)
            assert end==length,"Fixture should fit on one complete page"
            ends.append(end)
            image=Image.frombytes("RGB",(w,h),process.stdout.read(w*h*4),"raw","BGRX")
            pixels=image.load()
            left,top,right,bottom=w,h,-1,-1
            for y in range(h):
                for x in range(w):
                    if pixels[x,y]==(0,128,128):
                        left=min(left,x);right=max(right,x);top=min(top,y);bottom=max(bottom,y)
            assert right>=0,"Large scaled picture was lost"
            page_scale=min(w/11906,h/16838)
            assert abs((right-left+1)-9600*page_scale)<5,"Picture width was clipped or scaled twice"
            assert abs((bottom-top+1)-6400*page_scale)<5,"Picture height was clipped or scaled twice"
            assert 0<left<right<w-1 and 0<top<bottom<h-1
        assert len(set(ends))==1,"Pagination changed with viewport or DPI"
        process.stdin.close();assert process.wait(timeout=5)==0
    finally:
        timeout.cancel()
        if process.poll() is None:process.kill()
        process.wait()
print("PASS: RTF page fitting at preview/fullscreen sizes and 100%/200% DPI")
