"""User-registered local targets; no shell strings, arguments or broad scanning."""
from pathlib import Path
import ctypes,json,os,re,subprocess,threading,unicodedata,uuid

class TargetError(ValueError):pass
FILE_TYPES={'.pdf','.doc','.docx','.xls','.xlsx','.ppt','.pptx','.md','.txt','.csv','.json','.png','.jpg','.jpeg'}

def key(name):
    if not isinstance(name,str):raise TargetError('name_required')
    name=' '.join(unicodedata.normalize('NFKC',name).split())
    if not 1<=len(name)<=60 or any(ord(c)<32 or c in '/\\:' for c in name):raise TargetError('invalid_target_name')
    return name.casefold()

def target_path(value,kind):
    if not isinstance(value,str) or not value or len(value)>1024:raise TargetError('local_absolute_path_required')
    if any(ord(c)<32 for c in value) or value!=value.strip() or value.startswith(('\\\\','//')):raise TargetError('local_absolute_path_required')
    p=Path(value)
    if not p.is_absolute():raise TargetError('local_absolute_path_required')
    p=p.resolve(strict=True)
    if str(p).startswith(('\\\\','//')):raise TargetError('local_absolute_path_required')
    if kind=='folder':
        if not p.is_dir():raise TargetError('folder_required')
    elif kind=='app':
        if not p.is_file() or p.suffix.lower()!='.exe':raise TargetError('exe_required_no_arguments')
    elif kind=='file':
        if not p.is_file() or p.suffix.lower() not in FILE_TYPES:raise TargetError('unsupported_document_type')
    else:raise TargetError('kind_must_be_app_folder_or_file')
    return p

class TargetRegistry:
    def __init__(self,path):self.path=Path(path);self.lock=threading.RLock()
    def _read(self):
        if not self.path.exists():return []
        if self.path.stat().st_size>128000:raise TargetError('registry_too_large')
        value=json.loads(self.path.read_text(encoding='utf-8'))
        if value.get('version')!=1 or not isinstance(value.get('targets'),list) or len(value['targets'])>40:raise TargetError('invalid_registry')
        return value['targets']
    def _write(self,records):
        self.path.parent.mkdir(parents=True,exist_ok=True)
        temp=self.path.with_suffix('.tmp');temp.write_text(json.dumps(dict(version=1,targets=records),ensure_ascii=False,indent=2),encoding='utf-8');temp.replace(self.path)
    def list(self,local=False):
        with self.lock:
            rows=self._read();result=[]
            for r in rows:
                out={k:r[k] for k in ('id','name','aliases','kind','enabled')}
                if local:
                    out['path']=r['path']
                    try:target_path(r['path'],r['kind']);out['available']=True
                    except (OSError,ValueError):out['available']=False
                result.append(out)
            return result
    def save(self,body):
        # Registration happens only on the authenticated local configuration UI;
        # the model tool cannot register or change paths.
        if set(body)-{'id','name','aliases','kind','path','enabled'}:raise TargetError('unexpected_registration_fields')
        name=body.get('name','');key(name);name=name.strip()
        aliases=body.get('aliases',[])
        if not isinstance(aliases,list) or len(aliases)>8:raise TargetError('aliases_list_max_8')
        for alias in aliases:key(alias)
        tokens={key(name),*(key(a) for a in aliases)}
        p=target_path(body.get('path'),body.get('kind'))
        enabled=body.get('enabled',True)
        if type(enabled) is not bool:raise TargetError('enabled_boolean_required')
        with self.lock:
            rows=self._read();identifier=body.get('id')
            if identifier and not any(r['id']==identifier for r in rows):raise TargetError('target_id_not_found')
            for r in rows:
                if r['id']!=identifier and tokens&{key(r['name']),*(key(a) for a in r['aliases'])}:raise TargetError('target_name_or_alias_already_registered')
            if not identifier and len(rows)>=40:raise TargetError('target_limit_40')
            identifier=identifier or uuid.uuid4().hex
            row=dict(id=identifier,name=name,aliases=aliases,kind=body['kind'],path=str(p),enabled=enabled)
            self._write([r for r in rows if r['id']!=identifier]+[row]);return identifier
    def remove(self,identifier):
        with self.lock:
            rows=self._read();filtered=[r for r in rows if r['id']!=identifier]
            if len(filtered)==len(rows):raise TargetError('target_id_not_found')
            self._write(filtered)
    def toggle(self,identifier,enabled):
        if type(enabled) is not bool:raise TargetError('enabled_boolean_required')
        with self.lock:
            rows=self._read();row=next((r for r in rows if r['id']==identifier),None)
            if row is None:raise TargetError('target_id_not_found')
            row['enabled']=enabled;self._write(rows)
    def resolve(self,name):
        token=key(name)
        with self.lock:matches=[r for r in self._read() if token in {key(r['name']),*(key(a) for a in r['aliases'])}]
        if not matches:raise TargetError('target_not_registered_use_list')
        if len(matches)!=1:raise TargetError('ambiguous_target_name')
        r=matches[0]
        if not r['enabled']:raise TargetError('target_disabled')
        return r,target_path(r['path'],r['kind'])
    def open(self,name,cancel=lambda:False):
        row,p=self.resolve(name)
        if cancel():raise TargetError('task_cancelled')
        if row['kind']=='app':
            # Exact registered executable, no model-provided arguments.
            child=subprocess.Popen([str(p)],shell=False,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            result=dict(process_started=True,pid=child.pid)
        elif row['kind']=='folder':
            explorer=Path(os.environ.get('SystemRoot',r'C:\Windows'))/'explorer.exe'
            child=subprocess.Popen([str(explorer),str(p)],shell=False,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            result=dict(process_started=True,pid=child.pid)
        else:
            # Use the user's Windows document association. A path is data,
            # never a shell command or an executable script type.
            from ctypes import wintypes
            call=ctypes.windll.shell32.ShellExecuteW
            call.argtypes=[wintypes.HWND,wintypes.LPCWSTR,wintypes.LPCWSTR,wintypes.LPCWSTR,wintypes.LPCWSTR,ctypes.c_int]
            call.restype=ctypes.c_void_p
            code=call(None,'open',str(p),None,None,1)
            if not code or code<=32:raise TargetError('document_open_request_failed')
            result=dict(open_request_accepted=True)
        return dict(**result,target='registered',name=row['name'],kind=row['kind'],
            physical_window_verified=False,verification='Windows launch request only; no visual verification')

COMMON={
 'Weixin':('微信',['WeChat','Weixin']), 'WeChat':('微信',['WeChat','Weixin']),
 'pycharm64':('PyCharm',['派查姆']), 'chrome':('Chrome',['谷歌浏览器']),
 'msedge':('Edge',['微软浏览器']), 'WINWORD':('Word',['文档编辑器']),
 'EXCEL':('Excel',['表格']), 'POWERPNT':('PowerPoint',['PPT']), 'Code':('VS Code',['VSCode'])}

def discover():
    """Only known executable metadata, never conversation/title/file contents."""
    found={}
    names=','.join(COMMON)
    code='[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); Get-Process -Name '+names+' -ErrorAction SilentlyContinue | Select-Object -Unique ProcessName,Path | ConvertTo-Json -Compress'
    shell=Path(os.environ.get('SystemRoot',r'C:\Windows'))/'System32/WindowsPowerShell/v1.0/powershell.exe'
    run=subprocess.run([str(shell),'-NoProfile','-NonInteractive','-Command',code],capture_output=True,timeout=12,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    # Windows PowerShell redirected output uses the active OEM encoding unless
    # explicitly set. Set it above in a fixed script, never interpolate inputs.
    raw=run.stdout.decode('utf-8-sig',errors='replace').strip()
    try:rows=json.loads(raw) if raw else []
    except ValueError:rows=[]
    if isinstance(rows,dict):rows=[rows]
    for r in rows:
        process=r.get('ProcessName');path=r.get('Path')
        if process in COMMON and path:
            try:p=target_path(path,'app')
            except (ValueError,OSError):continue
            name,aliases=COMMON[process];found[name]=dict(name=name,aliases=aliases,kind='app',path=str(p),enabled=True)
    try:
        import winreg
        for exe,(name,aliases) in {**{k+'.exe':v for k,v in COMMON.items()},'notepad.exe':('记事本',['Notepad'])}.items():
            for hive in (winreg.HKEY_CURRENT_USER,winreg.HKEY_LOCAL_MACHINE):
                try:
                    with winreg.OpenKey(hive,r'Software\Microsoft\Windows\CurrentVersion\App Paths'+'\\'+exe) as k:
                        value,_=winreg.QueryValueEx(k,None)
                    p=target_path(os.path.expandvars(value.strip('"')),'app')
                    found.setdefault(name,dict(name=name,aliases=aliases,kind='app',path=str(p),enabled=True))
                except (OSError,ValueError):pass
    except ImportError:pass
    p=Path(os.environ.get('SystemRoot',r'C:\Windows'))/'System32/notepad.exe'
    if p.is_file():found.setdefault('记事本',dict(name='记事本',aliases=['Notepad'],kind='app',path=str(p),enabled=True))
    return list(found.values())
