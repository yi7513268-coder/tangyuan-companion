import hashlib,io,json,sys,tempfile
from pathlib import Path
from unittest.mock import patch
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from download_models import ensure_models

def setup_asset(root,name='model.task',data=b'public-model-bytes',digest=None):
 info=dict(url='https://storage.googleapis.com/mediapipe-models/public/model.task',bytes=len(data),sha256=digest or hashlib.sha256(data).hexdigest())
 (root/'manifest.json').write_text(json.dumps({name:info}),encoding='utf-8');return data

def test_verified_cache_does_not_use_network(tmp_path):
 data=setup_asset(tmp_path);(tmp_path/'model.task').write_bytes(data)
 with patch('download_models.urlopen',side_effect=AssertionError('unexpected network')):ensure_models(tmp_path)

def test_bad_hash_never_activates_new_asset(tmp_path):
 setup_asset(tmp_path,digest='0'*64)
 with patch('download_models.urlopen',return_value=io.BytesIO(b'public-model-bytes')):
  with pytest.raises(RuntimeError):ensure_models(tmp_path)
 assert not (tmp_path/'model.task').exists()

def test_manifest_cannot_escape_asset_directory(tmp_path):
 setup_asset(tmp_path,name='../escape.task')
 with patch('download_models.urlopen') as fetch:
  with pytest.raises(ValueError):ensure_models(tmp_path)
  fetch.assert_not_called()

def test_valid_asset_is_written_only_after_verification(tmp_path):
 data=setup_asset(tmp_path)
 with patch('download_models.urlopen',return_value=io.BytesIO(data)):ensure_models(tmp_path)
 assert (tmp_path/'model.task').read_bytes()==data
