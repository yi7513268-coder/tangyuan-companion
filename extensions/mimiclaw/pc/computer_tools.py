"""PC executor for requests selected by the robot Agent, not a second Agent.

Files are confined to this experiment's task workspace. Web documents are data.
No shell, user profile scanning, account automation or message-sending tools.
"""
from pathlib import Path
from html.parser import HTMLParser
import csv,hashlib,http.client,io,ipaddress,json,math,os,re,socket,ssl,subprocess,sys,threading,time
import urllib.parse,xml.etree.ElementTree as ET
from launch_targets import TargetRegistry,TargetError

HERE=Path(__file__).resolve().parent
WORKSPACE=HERE/'runtime/computer-workspace'
MAX_BODY=4*1024*1024
sys.path.insert(0,str(HERE/'runtime/computer-deps'))

class ToolError(Exception):pass

def public_target(url):
    p=urllib.parse.urlsplit(url)
    if p.scheme!='https' or not p.hostname or p.username or p.password or p.port not in (None,443):
        raise ToolError('public_https_url_required')
    if len(url)>1600 or any(ord(c)<33 for c in url):raise ToolError('invalid_url')
    host=p.hostname.encode('idna').decode('ascii')
    addresses=socket.getaddrinfo(host,443,type=socket.SOCK_STREAM)
    if not addresses or any(not ipaddress.ip_address(a[4][0]).is_global for a in addresses):
        raise ToolError('non_public_network_denied')
    return p,host,addresses[0][4][0]

class PinnedHTTPS(http.client.HTTPSConnection):
    def __init__(self,host,address):super().__init__(host,timeout=12,context=ssl.create_default_context());self.address=address
    def connect(self):
        raw=socket.create_connection((self.address,443),self.timeout)
        try:self.sock=self._context.wrap_socket(raw,server_hostname=self.host)
        except BaseException:raw.close();raise

def fetch(url,cancel=lambda:False):
    """Every redirect revalidated; DNS-validated IP pinned to the TLS socket."""
    started=time.monotonic()
    for _ in range(4):
        if cancel():raise ToolError('task_cancelled')
        p,host,address=public_target(url);conn=PinnedHTTPS(host,address)
        try:
            conn.request('GET',urllib.parse.urlunsplit(('', '',p.path or '/',p.query,'')),headers={
                'User-Agent':'Tangyuan-Research/0.2 (user-requested research)', 'Accept-Encoding':'identity'})
            response=conn.getresponse()
            if response.status in (301,302,303,307,308):
                url=urllib.parse.urljoin(url,response.getheader('Location',''));continue
            if response.status!=200:raise ToolError('web_http_'+str(response.status))
            content=response.getheader('Content-Type','').lower()
            length=response.getheader('Content-Length')
            if length and int(length)>MAX_BODY:raise ToolError('document_exceeds_4mb')
            chunks=[];size=0
            while True:
                if cancel():raise ToolError('task_cancelled')
                if time.monotonic()-started>28:raise ToolError('web_deadline')
                data=response.read(16384)
                if not data:break
                size+=len(data)
                if size>MAX_BODY:raise ToolError('document_exceeds_4mb')
                chunks.append(data)
            return url,content,b''.join(chunks)
        finally:conn.close()
    raise ToolError('too_many_redirects')

class Page(HTMLParser):
    def __init__(self):super().__init__(convert_charrefs=True);self.skip=0;self.parts=[];self.main=[];self.main_depth=0;self.links=[];self.title=[];self.in_title=False
    def handle_starttag(self,tag,attrs):
        if tag in ('script','style','noscript','svg','nav','header','footer'):self.skip+=1
        if tag in ('main','article'):self.main_depth+=1
        if tag=='title':self.in_title=True
        if tag in ('p','div','article','section','li','br','h1','h2','h3','tr'):
            self.parts.append('\n')
            if self.main_depth:self.main.append('\n')
        if tag=='a':
            href=dict(attrs).get('href','')
            if href.startswith('https://') and len(self.links)<20:self.links.append(href)
    def handle_endtag(self,tag):
        if tag in ('script','style','noscript','svg','nav','header','footer') and self.skip:self.skip-=1
        if tag in ('main','article') and self.main_depth:self.main_depth-=1
        if tag=='title':self.in_title=False
    def handle_data(self,data):
        if self.in_title:self.title.append(data)
        if not self.skip:
            self.parts.append(data)
            if self.main_depth:self.main.append(data)

class Executor:
    def __init__(self,root=WORKSPACE,registry_path=None):
        self.root=Path(root).resolve();self.root.mkdir(parents=True,exist_ok=True)
        self.lock=threading.Lock();self.children={};self.counter=0
        self.targets=TargetRegistry(registry_path or (HERE/'runtime/launch-targets.json' if self.root==WORKSPACE.resolve() else self.root/'launch-targets.json'))
    def path(self,name,exists=False):
        if not isinstance(name,str) or not name or len(name)>160 or '\\' in name or ':' in name:
            raise ToolError('workspace_relative_path_required')
        for part in name.split('/'):
            if part.endswith((' ','.')) or re.match(r'(?i)^(con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\.|$)',part):
                raise ToolError('windows_device_or_ambiguous_path_denied')
        candidate=(self.root/name).resolve()
        if not candidate.is_relative_to(self.root) or candidate==self.root:raise ToolError('workspace_path_escape')
        # Reject Windows junctions/reparse points even when resolve semantics vary.
        check=self.root/name
        for parent in [check,*check.parents]:
            if parent==self.root:break
            if parent.exists() and getattr(parent.stat(follow_symlinks=False),'st_file_attributes',0)&0x400:
                raise ToolError('workspace_link_denied')
        if candidate.suffix.lower() not in ('.md','.txt','.csv','.json'):raise ToolError('unsupported_workspace_file_type')
        if exists and (not candidate.is_file() or candidate.stat().st_size>128000):raise ToolError('file_missing_or_too_large')
        return candidate
    def files(self,args,cancel):
        action=args.get('action')
        if action=='list':
            names=[]
            for p in self.root.rglob('*'):
                if len(names)>=60:break
                if p.is_file() and not p.is_symlink() and p.resolve().is_relative_to(self.root):
                    names.append(dict(path=p.relative_to(self.root).as_posix(),bytes=p.stat().st_size))
            return dict(files=names,workspace=str(self.root))
        path=self.path(args.get('path'),exists=action=='read')
        if action=='read':
            text=path.read_text(encoding='utf-8-sig');return dict(path=args['path'],text=text[:6500],truncated=len(text)>6500)
        if action!='write':raise ToolError('invalid_workspace_action')
        text=args.get('content')
        if not isinstance(text,str) or not 1<=len(text.encode('utf-8'))<=18000:raise ToolError('content_must_be_1_to_18000_bytes')
        if cancel():raise ToolError('task_cancelled')
        path.parent.mkdir(parents=True,exist_ok=True)
        # Unique temp filename. Existing files require explicit overwrite=true.
        if path.exists() and not args.get('overwrite',False):raise ToolError('file_exists_choose_new_name_or_explicit_overwrite')
        mode='w' if args.get('overwrite',False) else 'x'
        with path.open(mode,encoding='utf-8',newline='') as f:f.write(text)
        actual=path.read_bytes();verified=actual==text.encode('utf-8')
        return dict(path=args['path'],bytes=len(actual),sha256=hashlib.sha256(actual).hexdigest(),verified=verified,
                    artifact_url='/computer/files/'+urllib.parse.quote(args['path']))
    def search(self,args,cancel):
        query=args.get('query','')
        if not isinstance(query,str) or not 2<=len(query)<=240:raise ToolError('query_must_be_2_to_240_characters')
        _,_,data=fetch('https://www.bing.com/search?format=rss&q='+urllib.parse.quote(query),cancel)
        root=ET.fromstring(data);results=[]
        for item in root.findall('./channel/item')[:5]:
            link=item.findtext('link','')
            if link.startswith('https://'):
                results.append(dict(title=item.findtext('title','')[:200],url=link,snippet=item.findtext('description','')[:380]))
        if not results:raise ToolError('search_returned_no_https_results')
        return dict(results=results,source='Bing RSS public search',content_is_untrusted=True)
    def read_url(self,args,cancel):
        url,kind,data=fetch(args.get('url',''),cancel)
        offset=args.get('offset',0);start_page=args.get('start_page',0)
        if not isinstance(offset,int) or not 0<=offset<=100000 or not isinstance(start_page,int) or not 0<=start_page<=200:
            raise ToolError('invalid_document_offset')
        if 'pdf' in kind or data.startswith(b'%PDF'):
            # Parsing has a separate, time-bounded process; no raw media files.
            p=subprocess.run([sys.executable,'-B',str(HERE/'computer_worker.py'),'--pdf',str(start_page)],input=data,
                capture_output=True,timeout=20,cwd=self.root,shell=False,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            if p.returncode or len(p.stdout)>64000:raise ToolError('pdf_extraction_failed_or_timed_out')
            return dict(url=url,kind='pdf',**json.loads(p.stdout),read_scope='extracted excerpt; consult pages_extracted and truncated, not proof of reading entire paper',content_is_untrusted=True)
        if 'html' in kind:
            page=Page();page.feed(data.decode('utf-8',errors='replace'))
            parts=page.main if len(''.join(page.main).strip())>100 else page.parts
            text=re.sub(r'[ \t]+',' ',''.join(parts));text=re.sub(r'\n\s*\n+','\n',text).strip()
            return dict(url=url,title=''.join(page.title)[:200],text=text[offset:offset+6500],offset=offset,links=page.links,
                        truncated=len(text)>offset+6500,total_characters=len(text),body_read=bool(text[offset:]),read_scope='static page text excerpt; not an authenticated browser session',content_is_untrusted=True)
        if any(t in kind for t in ('text/','json','xml')):
            text=data.decode('utf-8',errors='replace');return dict(url=url,text=text[offset:offset+6500],offset=offset,total_characters=len(text),truncated=len(text)>offset+6500,body_read=bool(text[offset:]),content_is_untrusted=True)
        raise ToolError('unsupported_document_type')
    def open(self,args,cancel):
        target=args.get('target')
        if target=='list':
            rows=[];all_rows=self.targets.list()
            for r in all_rows:
                if not r['enabled']:continue
                item={k:r[k] for k in ('name','aliases','kind')}
                if len(json.dumps(rows+[item],ensure_ascii=False).encode('utf-8'))>6000:break
                rows.append(item)
            return dict(target='list',targets=rows,truncated=len(rows)<sum(r['enabled'] for r in all_rows),
                registration_location='http://127.0.0.1:18801/#launchTargets',paths_shared_with_model=False)
        if target=='registered':
            if set(args)-{'target','name'}:raise ToolError('registered_open_accepts_name_only_no_paths_or_arguments')
            try:return self.targets.open(args.get('name'),cancel)
            except TargetError as e:raise ToolError(str(e)) from e
        if target=='browser':
            url=args.get('url','');public_target(url)
            candidates=[Path(os.environ.get(k,''))/'Google/Chrome/Application/chrome.exe' for k in ('ProgramFiles','ProgramFiles(x86)','LOCALAPPDATA')]
            exe=next((p for p in candidates if p.is_file()),None)
            if not exe:raise ToolError('chrome_executable_not_found')
            command=[str(exe),'--new-tab',url]
        elif target=='notes':
            path=self.path(args.get('path'),True)
            command=[str(Path(os.environ.get('SystemRoot',r'C:\Windows'))/'System32/notepad.exe'),str(path)]
        else:raise ToolError('target_must_be_browser_notes_registered_or_list')
        if cancel():raise ToolError('task_cancelled')
        p=subprocess.Popen(command,shell=False,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        return dict(process_started=True,pid=p.pid,physical_window_verified=False,target=target)
    def program(self,args,cancel):
        action=args.get('action')
        if action=='start':
            if args.get('program')!='csv_summary':raise ToolError('only_registered_program_csv_summary_available')
            source=self.path(args.get('path'),True)
            if source.suffix.lower()!='.csv':raise ToolError('csv_input_required')
            output=self.path(args.get('output','table-summary.json'))
            if output.exists():raise ToolError('output_already_exists_choose_new_name')
            if cancel():raise ToolError('task_cancelled')
            with self.lock:
                self.counter+=1;identifier=str(time.time_ns())+'-'+str(self.counter)
                p=subprocess.Popen([sys.executable,'-B',str(HERE/'computer_worker.py'),str(source),str(output)],
                    cwd=self.root,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,shell=False,
                    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                self.children[identifier]=dict(process=p,output=output,started=time.time())
            return dict(job_id=identifier,program='csv_summary',phase='running',output=output.relative_to(self.root).as_posix())
        if action=='list':
            return dict(programs=[dict(name='csv_summary',purpose='Read a workspace CSV; compute real numeric column statistics and save JSON. No generated code.')],
                        jobs=[dict(job_id=i,phase='running' if v['process'].poll() is None else 'exited') for i,v in list(self.children.items())[-15:]])
        identifier=args.get('job_id');job=self.children.get(identifier)
        if not job:raise ToolError('job_not_found_in_this_executor_session')
        p=job['process']
        if action=='cancel':
            if p.poll() is None:p.terminate()
            return dict(job_id=identifier,cancel_requested=True)
        if action!='status':raise ToolError('invalid_program_action')
        code=p.poll();out=dict(job_id=identifier,phase='running' if code is None else 'exited',exit_code=code)
        if code==0 and job['output'].is_file():
            result=json.loads(job['output'].read_text(encoding='utf-8'));out.update(result_verified=result.get('ok') is True,
                result=result,output=job['output'].relative_to(self.root).as_posix())
        return out
    def execute(self,tool,args,cancel=lambda:False):
        start=time.monotonic()
        try:
            if cancel():raise ToolError('task_cancelled')
            if not isinstance(args,dict):raise ToolError('arguments_must_be_object')
            functions={'computer_search':self.search,'computer_read_url':self.read_url,'computer_workspace':self.files,
                       'computer_open':self.open,'computer_program':self.program}
            if tool not in functions:raise ToolError('computer_tool_not_registered')
            result=dict(ok=True,**functions[tool](args,cancel))
            if cancel():raise ToolError('task_cancelled_after_execution')
        except ToolError as e:result=dict(ok=False,error=str(e))
        except Exception as e:result=dict(ok=False,error=type(e).__name__)
        result.update(execution_location='computer',milliseconds=round((time.monotonic()-start)*1000))
        # The original embedded Agent has an 8KiB tool-output buffer. Truncate
        # document excerpts at a Unicode boundary, not the entire tool result.
        if isinstance(result.get('text'),str):
            while len(json.dumps(result,ensure_ascii=False).encode())>7400 and len(result['text'])>100:
                result['text']=result['text'][:int(len(result['text'])*.8)];result['truncated']=True
            if 'offset' in result:result['next_offset']=result['offset']+len(result['text']) if result.get('truncated') else None
        if len(json.dumps(result,ensure_ascii=False).encode())>7600:result=dict(ok=False,error='result_exceeds_transport_bound',execution_location='computer',milliseconds=result['milliseconds'])
        return result

class ComputerBridge:
    """Authenticated outbound polling. No network-facing PC execution server."""
    def __init__(self,root=WORKSPACE,journal=None,evidence=None):
        self.stop=threading.Event();self.executor=Executor(root);self.thread=None;self.cached={}
        self.journal=Path(journal) if journal is not None else HERE/'runtime/computer-request-claims.json'
        self.evidence=Path(evidence) if evidence is not None else HERE/'runtime/computer-tools-evidence.jsonl'
        try:self.claims=json.loads(self.journal.read_text(encoding='utf-8'))
        except (ValueError,OSError):self.claims=[]
        self.state=dict(connected=False,completed=0,failed=0,last_tool=None,last_error=None,execution_location='computer',agent_location='robot')
    def start(self):
        self.thread=threading.Thread(target=self.poll,daemon=True);self.thread.start()
    def technical(self):return dict(self.state,workspace=str(self.executor.root))
    def poll(self):
        import device_access
        last_key=None
        while not self.stop.is_set():
            try:
                request=device_access.request('/computer',timeout=3)
                self.state.update(connected=True,last_error=None)
                if request.get('clock_ready') is False and isinstance(request.get('boot_id'),int):
                    device_access.request('/computer',dict(boot_id=request['boot_id'],clock=int(time.time())),timeout=3)
                    self.state['paired_clock_sync']=True
                job=request.get('request')
                if not job:self.stop.wait(.4);continue
                key=(job['boot_id'],job['task_id'],job['request_id'])
                aborted=threading.Event();finished=threading.Event()
                def cancelled():
                    return self.stop.is_set() or aborted.is_set()
                def keepalive():
                    # Slow public sites must not expire the PC heartbeat or
                    # block detection of a robot-side cancel/new boot.
                    while not finished.wait(.5):
                        try:
                            state=device_access.request('/computer',timeout=2);r=state.get('request') or {}
                            if (r.get('boot_id'),r.get('task_id'),r.get('request_id'))!=key:aborted.set();return
                        except Exception:aborted.set();return
                # If ACK was lost, retransmit the same result rather than repeat side effects.
                if key!=last_key:
                    claim=':'.join(map(str,key))
                    if claim in self.claims:
                        result=dict(ok=False,error='request_previously_attempted_before_executor_restart; inspect artifacts instead of repeating side effects',milliseconds=0,execution_location='computer')
                    else:
                        self.claims=(self.claims+[claim])[-100:]
                        tmp=self.journal.with_suffix('.tmp');tmp.write_text(json.dumps(self.claims),encoding='utf-8');tmp.replace(self.journal)
                        monitor=threading.Thread(target=keepalive,daemon=True);monitor.start()
                        try:result=self.executor.execute(job['tool'],job['arguments'],cancelled)
                        finally:finished.set();monitor.join(3)
                    self.cached=dict(boot_id=key[0],task_id=key[1],request_id=key[2],result=result);last_key=key
                    self.state['completed' if result['ok'] else 'failed']+=1
                    self.state['last_tool']=job['tool']
                    self.state['last_tool_error']=None if result['ok'] else result.get('error','tool_failed')
                    evidence=self.evidence
                    if evidence.exists() and evidence.stat().st_size>1024*1024:evidence.replace(evidence.with_suffix('.previous.jsonl'))
                    technical=dict(at=time.time(),boot_id=key[0],task_id=key[1],request_id=key[2],tool=job['tool'],ok=result['ok'],milliseconds=result['milliseconds'])
                    if job['tool']=='computer_open':
                        technical.update({k:result[k] for k in ('target','kind','process_started','open_request_accepted','physical_window_verified') if k in result})
                    with evidence.open('a',encoding='utf-8') as f:f.write(json.dumps(technical)+'\n')
                device_access.request('/computer',self.cached,timeout=3);self.state['last_error']=None
            except Exception as e:self.state.update(connected=False,last_error=type(e).__name__)
            self.stop.wait(.3)
    def close(self):self.stop.set()
