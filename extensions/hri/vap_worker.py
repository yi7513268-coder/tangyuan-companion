"""Pinned upstream neural inference, unmodified checkpoints, CPU only."""
import base64, contextlib, hashlib, json, os
from pathlib import Path
import sys, time, types
ROOT=Path(os.environ.get('TANGYUAN_VAP_ASSETS',str(Path(__file__).resolve().parent/'assets')))
os.environ.update(HF_HUB_OFFLINE='1',HF_HOME=str(ROOT/'cache/huggingface'),TORCH_HOME=str(ROOT/'cache/torch'),OMP_NUM_THREADS='1',MKL_NUM_THREADS='1')

def main():
    print('HRI_STAGE importing_torch',file=sys.stderr,flush=True)
    import torch
    import numpy as np
    torch.set_num_threads(1);torch.set_num_interop_threads(1)
    print('HRI_STAGE importing_author_code',file=sys.stderr,flush=True)
    package=types.ModuleType('maai');package.__path__=[str(ROOT/'upstream/maai/src/maai')];sys.modules['maai']=package
    from maai.model import Maai
    from maai.input import Chunk
    manifest=json.loads((ROOT/'models/manifest.json').read_text())
    for name,info in manifest['files'].items():
        if hashlib.sha256((ROOT/'models'/name).read_bytes()).hexdigest()!=info['sha256']:raise ValueError('Checkpoint checksum mismatch')
    print('HRI_STAGE loading_pinned_weights',file=sys.stderr,flush=True)
    with contextlib.redirect_stdout(sys.stderr):
        model=Maai(mode='vap',lang='ch',audio_ch1=Chunk(),frame_rate=10,device='cpu',cpc_model=str(ROOT/'models/60k_epoch4-d0f474de.pt'),cache_dir=str(ROOT/'cache/huggingface'),local_model=str(ROOT/'models/vap_mono_state_dict_ch_10hz_20000msec.pt'),mc=False)
    bc=None;bc_error=None
    try:
        info=json.loads((Path(__file__).parent/'sources/20261024/bc-checkpoint.json').read_text())
        checkpoint=Path(__file__).parent/'models'/info['file']
        if hashlib.sha256(checkpoint.read_bytes()).hexdigest()!=info['sha256']:raise ValueError('Backchannel checksum mismatch')
        with contextlib.redirect_stdout(sys.stderr):
            bc=Maai(mode='bc',lang='ch',audio_ch1=Chunk(),audio_ch2=Chunk(),frame_rate=10,context_len_sec=20,device='cpu',cpc_model=str(ROOT/'models/60k_epoch4-d0f474de.pt'),cache_dir=str(ROOT/'cache/huggingface'),local_model=str(checkpoint),mc=False)
    except Exception as e:bc_error=type(e).__name__
    buffer=b'';last_key=None;last_sequence=None;context_frames=0;bc_context=0;bc_enabled=False
    print(json.dumps(dict(ready=True,variant=model.variant,backchannel_loaded=bc is not None,backchannel_error=bc_error)),flush=True)
    for line in sys.stdin:
        try:
            r=json.loads(line)
            if r.get('reset') is True:
                model.reset_runtime_state();buffer=b'';last_key=None;last_sequence=None;context_frames=0;bc_context=0
                if bc:bc.reset_runtime_state()
                print(json.dumps(dict(ok=True,reset=True)),flush=True);continue
            key=(r['boot_id'],r['speech_epoch']);seq=r['sequence'];count=r['frames'];data=base64.b64decode(r['pcm'],validate=True)
            if len(data)!=count*640 or not 0<=count<=50:raise ValueError('Invalid PCM framing')
            new=count if last_sequence is None else seq-last_sequence
            discontinuity=key!=last_key or new<0 or new>count
            dropped=0
            # An initial/recovered stream may include a second of old PCM.
            # Explicit resynchronization is preferable to permanently chasing
            # it. Reset context, retain the latest 100ms, expose dropped frames,
            # and suppress BC action predictions until 2s new context arrives.
            if discontinuity:
                dropped=max(0,(count if discontinuity else new)-5);new=min(5,count);discontinuity=True
            if discontinuity:
                model.reset_runtime_state();buffer=b'';context_frames=0;bc_context=0
                if bc:bc.reset_runtime_state()
            enabled=bool(r.get('backchannel'))
            if enabled!=bc_enabled:
                if bc:bc.reset_runtime_state()
                bc_context=0;bc_enabled=enabled
            last_key=key;last_sequence=seq
            if new:buffer+=data[-new*640:]
            output=None;bc_output=None;started=time.monotonic();calls=0
            while len(buffer)>=3200:
                pcm,buffer=buffer[:3200],buffer[3200:];x=np.frombuffer(pcm,dtype='<i2').astype(np.float32)/32768
                with contextlib.redirect_stdout(sys.stderr):model.process(x,np.zeros_like(x))
                if bc and r.get('backchannel'):
                    with contextlib.redirect_stdout(sys.stderr):bc.process(x,np.zeros_like(x))
                    bc_context+=5
                    while not bc.result_dict_queue.empty():bc_output=bc.result_dict_queue.get_nowait()
                calls+=1
                context_frames+=5
                while not model.result_dict_queue.empty():output=model.result_dict_queue.get_nowait()
            result=dict(ok=True,boot_id=key[0],speech_epoch=key[1],sequence=seq-len(buffer)//640,calls=calls,discontinuity=discontinuity,dropped_frames=dropped,context_frames=context_frames,bc_context_frames=bc_context,milliseconds=round((time.monotonic()-started)*1000,2))
            if output is not None:result.update(p_now=float(output['p_now']),p_future=float(output['p_future']),vad=float(output['vad']))
            if bc_output is not None and bc_context>=100:result['p_bc']=float(bc_output['p_bc'])
            result['backchannel_loaded']=bc is not None
            print(json.dumps(result),flush=True)
        except Exception as e:print(json.dumps(dict(ok=False,error_type=type(e).__name__)),flush=True)

if __name__=='__main__':
    try:main()
    except Exception as e:print(json.dumps(dict(ready=False,error_type=type(e).__name__)),flush=True)
