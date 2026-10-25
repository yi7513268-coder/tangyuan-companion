"""Device token protected with current-user Windows DPAPI; no cloud keys stored."""
from pathlib import Path
import ctypes,json,ipaddress,time
from ctypes import wintypes
from urllib.request import Request,build_opener,ProxyHandler
from urllib.error import HTTPError
HERE=Path(__file__).resolve().parent
PRIVATE=HERE/'runtime/private';PUBLIC=HERE/'runtime/board-device.json'
class Blob(ctypes.Structure):_fields_=[('cbData',wintypes.DWORD),('pbData',ctypes.POINTER(ctypes.c_ubyte))]
def crypt(data,decrypt=False):
    array=(ctypes.c_ubyte*len(data)).from_buffer_copy(data);source=Blob(len(data),array);out=Blob()
    function=ctypes.windll.crypt32.CryptUnprotectData if decrypt else ctypes.windll.crypt32.CryptProtectData
    if not function(ctypes.byref(source),None,None,None,None,1,ctypes.byref(out)):raise ctypes.WinError()
    try:return ctypes.string_at(out.pbData,out.cbData)
    finally:ctypes.windll.kernel32.LocalFree(out.pbData)
def store(ip,token,image_sha256):
    PRIVATE.mkdir(parents=True,exist_ok=True)
    (PRIVATE/'device-token.dpapi').write_bytes(crypt(token.encode()))
    PUBLIC.write_text(json.dumps(dict(ip=ip,port=80,image_sha256=image_sha256,token_protected=True),indent=2),encoding='utf-8')
def load():
    d=json.loads(PUBLIC.read_text(encoding='utf-8'));address=ipaddress.ip_address(d['ip'])
    if address.version!=4 or not address.is_private or address.is_loopback:raise ValueError('Device is not a private IPv4 endpoint')
    token=crypt((PRIVATE/'device-token.dpapi').read_bytes(),True).decode()
    return d,token
def raw(path,body=None,timeout=5,port=80,headers=None):
    base=path.split('?',1)[0]
    if base not in ('/status','/task','/cancel','/control','/gesture','/preview','/audio','/play','/voice','/computer','/agent','/hri_audio','/hri_prediction','/voice_control') or port not in (80,81):raise ValueError('Unregistered device endpoint')
    if '?' in path and (base!='/audio' or not path.split('?',1)[1].startswith('id=') or not path.split('=',1)[1].isdigit()):raise ValueError('Invalid audio identifier')
    d,token=load();req=Request(f"http://{d['ip']}:{port}{path}",data=body,
        headers={'Content-Type':'application/octet-stream','Authorization':'Bearer '+token,**(headers or {})})
    # A dropped response is ambiguous for commands: never replay mutations.
    # Read-only polls may reconnect once; HRI still measures total latency and
    # rejects predictions that arrive outside its freshness budget.
    read_only=body is None
    if base=='/agent' and body:
        try:
            obj=json.loads(body);read_only=obj.get('tool')=='get_current_time' or (obj.get('tool') in ('agent_files','agent_schedule') and obj.get('arguments',{}).get('action') in ('read','list'))
        except (ValueError,TypeError):pass
    for attempt in range(2 if read_only else 1):
        try:
            with build_opener(ProxyHandler({})).open(req,timeout=timeout) as r:
                value=r.read(4*1024*1024+1)
                if len(value)>4*1024*1024:raise ValueError('Device response bound')
                return value
        except HTTPError:raise
        except OSError:
            if not read_only or attempt:raise
            time.sleep(.05)
def request(path='/status',body=None,timeout=5,port=80):
    return json.loads(raw(path,json.dumps(body,ensure_ascii=False).encode() if body is not None else None,timeout,port,{'Content-Type':'application/json'}))
