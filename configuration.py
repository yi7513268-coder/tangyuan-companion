"""Validate local settings before starting models or hardware operations."""

from math import isfinite

DEFAULTS = {
    'port': 18780, 'pan_sign': -1, 'tilt_sign': 1,
    'pan_limits': [30,150], 'tilt_limits': [100,130],
    'poll_interval': .03, 'gesture_hold_seconds': .5, 'gesture_cooldown_seconds': 2.5,
    'camera_mode': 'inference', 'vision_model_id': 4, 'start_paused': True,
    'follow_pan_speed': 36, 'follow_tilt_speed': 16, 'follow_smoothing': .75,
    'continuous_follow': False,
}


def validate_config(raw: dict) -> dict:
    if not isinstance(raw, dict):
        raise ValueError('config.json must contain an object')
    unknown = set(raw) - set(DEFAULTS)
    if unknown:
        raise ValueError('Unknown configuration fields: '+', '.join(sorted(unknown)))
    config = {**DEFAULTS, **raw}
    for key, low, high in [('port',1024,65535), ('vision_model_id',1,255)]:
        value = config[key]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f'{key} must be an integer in {low}..{high}')
    for key in ('pan_sign','tilt_sign'):
        if type(config[key]) is not int or config[key] not in (-1,1):
            raise ValueError(key+' must be -1 or 1')
    for key, low, high in [('poll_interval',.03,1), ('gesture_hold_seconds',.15,2),
                            ('gesture_cooldown_seconds',.1,10),('follow_pan_speed',10,60),
                            ('follow_tilt_speed',5,30),('follow_smoothing',.2,1)]:
        value = config[key]
        if type(value) not in (int,float) or not isfinite(value) or not low <= value <= high:
            raise ValueError(f'{key} must be a finite number in {low}..{high}')
    for key, low, center, high in [('pan_limits',30,90,150), ('tilt_limits',100,115,130)]:
        value = config[key]
        if (not isinstance(value,(list,tuple)) or len(value)!=2
                or any(type(v) is not int for v in value)
                or not low <= value[0] <= center <= value[1] <= high or value[0]==value[1]):
            raise ValueError(f'{key} must contain increasing integer limits in {low}..{high}, including {center}')
        config[key] = list(value)
    if config['camera_mode'] not in ('capture','inference'):
        raise ValueError('camera_mode must be capture or inference')
    for key in ('start_paused','continuous_follow'):
        if type(config[key]) is not bool:raise ValueError(key+' must be boolean')
    return config
