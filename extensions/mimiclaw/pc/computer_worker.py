"""Registered, fixed CSV program. No eval, user code or shell execution."""
import csv,json,math,statistics,sys,io
from pathlib import Path

def summarize(source):
    with Path(source).open(encoding='utf-8-sig',newline='') as f:
        rows=list(csv.DictReader(f))
    if not rows or len(rows)>10000:raise ValueError('CSV row count must be 1..10000')
    columns={}
    for name in list(rows[0])[:40]:
        values=[];missing=0;non_numeric=0
        for row in rows:
            raw=(row.get(name) or '').strip()
            if not raw:missing+=1;continue
            try:
                n=float(raw)
                if not math.isfinite(n):raise ValueError()
                values.append(n)
            except ValueError:non_numeric+=1
        result=dict(numeric_count=len(values),missing=missing,non_numeric=non_numeric)
        if values:result.update(min=min(values),max=max(values),mean=statistics.mean(values),median=statistics.median(values))
        columns[name]=result
    return dict(ok=True,rows=len(rows),columns=columns,verification='parsed actual CSV; finite numeric values only')

if __name__=='__main__':
    if sys.argv[1]=='--pdf':
        sys.path.insert(0,str(Path(__file__).resolve().parent/'runtime/computer-deps'))
        from pypdf import PdfReader
        data=sys.stdin.buffer.read(4*1024*1024+1)
        if len(data)>4*1024*1024:raise ValueError('PDF too large')
        reader=PdfReader(io.BytesIO(data));pages=len(reader.pages);chunks=[];size=0;start=int(sys.argv[2])
        if not 0<=start<pages:raise ValueError('Page outside document')
        for page in reader.pages[start:min(start+3,pages)]:
            text=page.extract_text() or '';chunks.append(text);size+=len(text)
            if size>7000:break
        text='\n'.join(chunks)
        if len(text.strip())<40:raise ValueError('No extractable text')
        # A page may exceed the embedded transport excerpt. Report that gap;
        # next_page never claims unread parts of a long page were inspected.
        result=dict(text=text[:6500],pages=pages,start_page=start,pages_extracted=len(chunks),
            next_page=start+len(chunks) if start+len(chunks)<pages else None,
            truncated=len(text)>6500 or start>0 or start+len(chunks)<pages,body_read=True)
        sys.stdout.buffer.write(json.dumps(result,ensure_ascii=False).encode('utf-8'))
    else:
        result=summarize(sys.argv[1])
        with Path(sys.argv[2]).open('x',encoding='utf-8') as f:json.dump(result,f,ensure_ascii=False,indent=2,allow_nan=False)
