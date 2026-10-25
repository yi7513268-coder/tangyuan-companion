"""NAACL2025 BC timing -> bounded neutral nod; explicit dialogue only.

The Chinese checkpoint is the authors' later release. Nod embodiment, 0.5
threshold and 10s refractory interval are engineering adaptations, not a
replication of the paper's experimental settings. No vocal backchannels.
"""
import math,time

class Listener:
    def __init__(self):
        self.mode='off';self.boot=None;self.started=None;self.last=-math.inf;self.above=False;self.recording_addressed=False
        self.state=dict(mode=self.mode,reason='not_started',candidates=0,submitted=0,hardware_completed=0,request_id=None,physical_acceptance=False,
            evidence_source='author_audio_BC_model_and_explicit_dialogue',visual_required=False,recording_observations=0,blocked_counts={})
    def set_mode(self,value):
        if value not in ('off','observe','live'):raise ValueError('Invalid listener mode')
        self.mode=value;self.state['mode']=value;self.started=None;self.above=False;self.recording_addressed=False
    def decide(self,s,visual,prediction,now=None,wall=None):
        now=time.monotonic() if now is None else now;wall=time.time() if wall is None else wall
        a=s.get('audio',{});r=s.get('robot',{})
        if self.boot!=s.get('boot_id'):self.boot=s.get('boot_id');self.started=None;self.above=False;self.last=now;self.recording_addressed=False
        if not a.get('recording'):self.started=None;self.above=False;self.recording_addressed=False
        elif self.started is None:
            self.started=now;self.recording_addressed=bool(a.get('dialog_open'))
        if a.get('muted'):return 'quiet'
        if self.mode=='off':return 'off'
        if s.get('busy') or a.get('playing') or a.get('asr_pending') or a.get('say_text'):return 'robot_busy'
        if not a.get('enabled') or not a.get('recording') or not (self.recording_addressed or a.get('dialog_open')):return 'no_addressed_live_speech'
        # The published BC component consumes speech, not facial landmarks.
        # A fresh face estimate can be displayed, but is not proof of intent
        # and must not disable an explicitly addressed audio dialogue.
        if r.get('command_id',0)!=r.get('completed_id',0):return 'motion_pending'
        if self.started is None or now-self.started<1.5:return 'speech_too_short'
        p=prediction.get('p_bc');at=prediction.get('bc_at')
        if not prediction.get('backchannel_loaded') or at is None or not 0<=wall-at<.3 or prediction.get('bc_boot')!=s.get('boot_id') or prediction.get('bc_epoch')!=a.get('speech_epoch'):return 'stale_or_missing_model'
        if not isinstance(p,(int,float)) or not math.isfinite(p) or not 0<=p<=1:return 'invalid_probability'
        high=p>=.5;edge=high and not self.above;self.above=high
        if not high:return 'below_threshold'
        if not edge:return 'same_peak'
        if now-self.last<10:return 'cooldown'
        self.last=now;self.state['candidates']+=1
        return 'nod' if self.mode=='live' else 'observed_candidate'
    def update(self,s,v,p,dispatch):
        if self.boot!=s.get('boot_id'):self.state['request_id']=None
        pending=self.state.get('request_id');r=s.get('robot',{})
        if pending and r.get('completed_id')==pending:
            self.state['last_hardware_result']=r.get('command_result');self.state['request_id']=None
            if r.get('command_result')==1:self.state['hardware_completed']+=1
        reason=self.decide(s,v,p);self.state['reason']=reason
        if s.get('audio',{}).get('recording') and self.mode!='off':
            self.state['recording_observations']+=1
            if reason not in ('nod','observed_candidate'):
                counts=self.state['blocked_counts'];counts[reason]=counts.get(reason,0)+1
        if reason=='nod':
            try:
                result=dispatch('/control',dict(action='nod'))
                if result.get('ok'):
                    self.state['submitted']+=1;self.state['request_id']=result.get('request_id');self.state['reason']='requested_not_physically_verified'
                else:self.state['reason']='request_rejected'
            except OSError:self.state['reason']='request_receipt_unknown_no_retry'
        return dict(self.state,blocked_counts=dict(self.state['blocked_counts']))
