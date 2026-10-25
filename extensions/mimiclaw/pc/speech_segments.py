"""Bound long synthesis units without changing, dropping or repeating text."""
def split_speech(text,limit=40):
    parts=[]
    while text:
        cost=0;end=0;boundary=0
        for i,c in enumerate(text):
            weight=2 if '\u3400'<=c<='\u9fff' else 1
            if cost+weight>limit:break
            cost+=weight;end=i+1
            if c in '。！？!?；;，,\n ' and cost>=limit//3:boundary=end
        if end==len(text):parts.append(text);break
        cut=boundary or end
        parts.append(text[:cut]);text=text[cut:]
    return parts
