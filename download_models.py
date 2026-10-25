"""Download pinned Google models and verify checksums from manifest.json."""

import hashlib
import json
from pathlib import Path
from urllib.request import urlopen


def ensure_models(directory=None) -> None:
    directory = Path(directory) if directory is not None else Path(__file__).resolve().parent/'models'
    manifest = json.loads((directory/'manifest.json').read_text(encoding='utf-8'))
    for name,metadata in manifest.items():
        from urllib.parse import urlsplit
        url=urlsplit(metadata['url'])
        if Path(name).name!=name or url.scheme!='https' or url.hostname!='storage.googleapis.com':raise ValueError('Unexpected model source')
        target = directory/name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest()==metadata['sha256']:
            print(name+': verified')
            continue
        with urlopen(metadata['url'],timeout=60) as response:
            limit=int(metadata['bytes'])
            if not 0<limit<=16*1024*1024:raise ValueError('Asset size bound')
            content = response.read(limit+1)
        if len(content)!=metadata['bytes'] or hashlib.sha256(content).hexdigest()!=metadata['sha256']:
            raise RuntimeError('Model checksum mismatch: '+name)
        temporary = target.with_suffix(target.suffix+'.tmp')
        temporary.write_bytes(content)
        temporary.replace(target)
        print(name+': downloaded and verified')

if __name__ == '__main__':
    ensure_models()
