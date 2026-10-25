"""Close only a Popen child's descendants, including Windows venv redirectors.
Never enumerate all Python processes or match unrelated command lines.
"""
import subprocess
import psutil
def close_owned(process):
    if process is None or process.poll() is not None:return
    try:children=psutil.Process(process.pid).children(recursive=True)
    except psutil.NoSuchProcess:children=[]
    # The Popen process is still alive, so this is its actual current tree.
    for child in reversed(children):
        try:child.terminate()
        except psutil.NoSuchProcess:pass
    process.terminate()
    _,alive=psutil.wait_procs(children,timeout=3)
    for child in alive:
        try:child.kill()
        except psutil.NoSuchProcess:pass
    try:process.wait(3)
    except subprocess.TimeoutExpired:process.kill();process.wait(3)
