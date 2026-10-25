"""Content-free failure evidence shared by the isolated speech workers."""
from pathlib import Path
import traceback

def failure_details(error):
    # Exception messages can contain spoken text, paths or credentials: classify,
    # but never return/log the message itself.
    message=str(error).lower()
    reason='generation_failure'
    for phrase,category in (
        ('out of memory','gpu_memory'),('内存余量不足','host_memory'),
        ('expected all tensors','device_mismatch'),('same device','device_mismatch'),
        ('size mismatch','tensor_shape'),('shapes cannot','tensor_shape'),
        ('cuda error','cuda_runtime'),('non-finite','invalid_audio'),
        ('empty','empty_audio'),('silent','invalid_audio'),
        ('speech text bound','text_bound')):
        if phrase in message:reason=category;break
    return dict(error_type=type(error).__name__,reason=reason,
        frames=[dict(file=Path(f.filename).name,function=f.name,line=f.lineno)
                for f in traceback.extract_tb(error.__traceback__)[-8:]])

class SpeechWorkerError(RuntimeError):
    def __init__(self,details):
        self.details={k:details[k] for k in ('error_type','reason','frames') if k in details}
        super().__init__(self.details.get('reason','worker_failure'))
