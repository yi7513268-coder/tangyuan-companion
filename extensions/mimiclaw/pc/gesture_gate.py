"""Fresh-frame gesture admission; no images retained."""
class GestureGate:
    def __init__(self):self.candidate='';self.since=0;self.held='';self.seen=-1e9;self.fired=-1e9;self.sequence=None
    def observe(self,name,score,sequence,age_ms,now):
        if sequence==self.sequence:return None
        self.sequence=sequence
        if age_ms>400 or name not in ('Thumb_Up','Victory','Open_Palm') or score<.7:
            self.candidate=''
            if now-self.seen>.35:self.held=''
            return None
        if now-self.seen>.5:self.candidate=''
        self.seen=now
        if name!=self.candidate:self.candidate=name;self.since=now
        if name==self.held or now-self.since<.65 or now-self.fired<3:return None
        self.held=name;self.fired=now;return name
