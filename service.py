"""Single hardware owner, local inference, and loopback diagnostic controls."""

from __future__ import annotations

import asyncio
from collections import deque
import copy
import json
from math import ceil
from pathlib import Path
import queue
import threading
import time

import cv2
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, Response, StreamingResponse
from watcherobot.errors import CommandError,JobError,WatcheRobotError,ConnectionTimeoutError

from control import FaceFollower, GestureGate, GESTURE_THRESHOLDS, CONTROL_GESTURES
from perception import Perception
from calibration import image_shift, axis_sign
from configuration import validate_config


class CameraUnavailableError(RuntimeError):
    """A known acquisition/transport failure, distinct from a model failure."""


class VisionService:
    def __init__(self, robot, logger, root: Path, config: dict) -> None:
        config = validate_config(config)
        self.robot, self.logger, self.root, self.config = robot, logger, root, config
        self.follower = FaceFollower(pan_sign=config['pan_sign'], tilt_sign=config['tilt_sign'],
                                    pan_limits=tuple(config['pan_limits']),
                                    tilt_limits=tuple(config['tilt_limits']),
                                    pan_speed=config['follow_pan_speed'],tilt_speed=config['follow_tilt_speed'],
                                    smoothing=config['follow_smoothing'])
        self.gate = GestureGate(hold_seconds=config['gesture_hold_seconds'],
                                cooldown=config['gesture_cooldown_seconds'],continuation_thresholds={'Thumb_Up':.5})
        self._lock = threading.Lock()
        self._image_ready = threading.Condition(self._lock)
        self._stop = threading.Event()
        self._shutdown = threading.Event()
        self._desired_follow=not config['start_paused'];self._desired_gestures=not config['start_paused']
        self._preference_saved=False;self._preference_error=''
        try:
            preference=json.loads((self.root/'data/visual-preferences.json').read_text(encoding='utf-8'))
            if type(preference.get('follow_and_gestures')) is bool:
                self._desired_follow=preference['follow_and_gestures'];self._preference_saved=True
                self._desired_gestures=self._desired_follow
            for key,attribute in (('follow_enabled','_desired_follow'),('gestures_enabled','_desired_gestures')):
                if type(preference.get(key)) is bool:
                    setattr(self,attribute,preference[key]);self._preference_saved=True
        except FileNotFoundError:pass
        except (OSError,ValueError,TypeError,AttributeError):self._preference_error='视觉开关记录不可用，使用原启动设置'
        self._pause = threading.Event()
        if not self._desired_follow:
            self._pause.set()
        self._armed = self._desired_follow or self._desired_gestures
        self._pause_reason='none' if self._desired_follow else 'follow_only' if self._desired_gestures else 'user'
        self._fresh_camera_frames=0;self._retryable_fault=False;self._restarting=False
        self._gesture_hold_active=threading.Event();self._gesture_release_at=0.0
        self._recovery_attempts=0;self._recovery_delay=3.0
        self._inference_retry_after=0.0
        self._pause_revision = 0
        self._paused_ack = False
        self._commands: queue.Queue[tuple[str,int]] = queue.Queue(maxsize=1)
        self._gestures: queue.Queue[tuple[str,int,float]] = queue.Queue(maxsize=1)
        self._social_cue=None;self._last_social_cue=-1e9
        self._observations: queue.Queue = queue.Queue(maxsize=1)
        self._thread: threading.Thread | None = None
        self._camera_thread: threading.Thread | None = None
        self._camera_fault = threading.Event()
        self._camera_stalled = threading.Event()
        self._camera_error: str | None = None
        self._camera_reopen_times = deque()
        self._raw_jpeg: bytes | None = None
        self._raw_requested_at = 0.0
        self._motion_finished_at = 0.0
        self._follow_ready_after = 0.0
        self._follow_move = None
        self._interaction_move = False
        self._interaction_kind = 'sweep'
        self._search_centering = False
        self._jpeg: bytes | None = None
        self._frame_at = 0.0
        self._session = None
        self._pending_inference_start = False
        self._owns_motion = False
        self._owns_animation = False
        self._owns_light = False
        self._events = deque(maxlen=30)
        self.observation_listener = None
        self.visual_feedback = None
        self.social_response_allowed = lambda: True
        self.gesture_response_allowed = lambda name: True
        self.follow_response_allowed = lambda: True
        self.interaction_search = lambda sight,observation,acquired_at: None
        self.interaction_completed = lambda kind,pan,tilt,at,revision: None
        self._state = {'phase': 'starting', 'frames': 0, 'faces': 0, 'gesture': None,
                       'score': 0.0, 'fps': 0.0, 'inference_ms': 0.0, 'error': None,
                       'pan_deg': 90, 'tilt_deg': 115, 'camera_mode': None, 'gesture_events': 0,
                       'servo_commands': 0, 'last_action': None, 'brightness': 0.0,
                       'pose_confirmed': False, 'tracking_status': 'paused' if self._pause.is_set() else 'no_face',
                       'follow_active': False, 'follow_target': None, 'camera_request_ms': None, 'follow_command_ms': None,
                       'follow_job_ms': None, 'control_frame_age_ms': None}
        self._state.update(camera_timeouts=0,camera_busy_retries=0,camera_recoveries=0,camera_reopens=0,camera_last_error=None)
        self._state['gesture_status']='no_gesture' if self._desired_follow else 'actions_disabled'

    def snapshot(self) -> dict:
        with self._lock:
            state = copy.deepcopy(self._state)
            state['events'] = list(self._events)
            state['frame_age_seconds'] = round(time.monotonic()-self._frame_at,2) if self._frame_at else None
        state['armed'] = self._armed
        state['following'] = self._armed and not self._pause.is_set()
        state['interaction_hold'] = not self.follow_response_allowed()
        state['motion_revision'] = self._revision()
        state.update(follow_requested=self._desired_follow,pause_reason=self._pause_reason,
                     gestures_requested=self._desired_gestures,gestures_enabled=self._gestures_available(),
                     continuous_follow=self.config['continuous_follow'],gesture_temporary_hold=self._gesture_hold_active.is_set(),
                     recovery_attempts=self._recovery_attempts,fresh_recovery_frames=self._fresh_camera_frames,
                     preference_error=self._preference_error)
        state['axis_signs'] = {'pan': self.follower.pan_sign, 'tilt': self.follower.tilt_sign}
        return state

    def jpeg(self) -> bytes | None:
        with self._lock:
            return self._jpeg

    def event(self, message: str) -> None:
        self.logger.info(message)
        with self._lock:
            self._events.append({'time': time.strftime('%H:%M:%S'), 'message': message})

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name='robot-vision-owner', daemon=False)
        self._thread.start()

    def _save_preference_locked(self,enabled:bool,gestures:bool|None=None) -> None:
        self._desired_follow=enabled;self._preference_saved=True
        if gestures is not None:self._desired_gestures=gestures
        try:
            path=self.root/'data/visual-preferences.json';path.parent.mkdir(parents=True,exist_ok=True)
            temporary=path.with_suffix('.json.tmp')
            temporary.write_text(json.dumps({'follow_and_gestures':enabled and self._desired_gestures,
                'follow_enabled':enabled,'gestures_enabled':self._desired_gestures}),encoding='utf-8');temporary.replace(path)
            self._preference_error=''
        except OSError as exc:self._preference_error='视觉开关尚未保存：'+type(exc).__name__

    def pause(self, *, disarm: bool = True, cause:str='user') -> None:
        # This flag is visible immediately even while a move is awaiting completion.
        with self._lock:
            self._pause_revision += 1
            self._pause.set()
            self._pause_reason=cause
            if cause=='user':self._save_preference_locked(False,False)
            if disarm:
                self._armed = False

    def _revision(self) -> int:
        with self._lock:
            return self._pause_revision

    def request(self, command: str) -> None:
        if command == 'pause':
            self.pause()
            return
        if command in {'follow-off','gestures-on','gestures-off'}:
            with self._lock:
                if self._stop.is_set() or self._state['phase'] in {'error','stopped'}:
                    raise RuntimeError('Application unavailable; restore connection first')
                self._pause_revision+=1  # Invalidate an older gesture or queued resume immediately.
                if command=='follow-off':
                    self._save_preference_locked(False)
                    self._pause.set();self._paused_ack=False
                else:self._save_preference_locked(self._desired_follow,command=='gestures-on')
                self._armed=self._desired_follow or self._desired_gestures
                if self._pause_reason in {'none','user','follow_only'}:
                    self._pause_reason=('none' if self._desired_follow else
                                        'follow_only' if self._desired_gestures else 'user')
            return
        if command not in {'resume','recenter','flip_pan','flip_tilt','calibrate'}:
            raise ValueError('Unknown control')
        if command=='resume' and self._camera_stalled.is_set():
            raise RuntimeError('Camera is recovering; wait for a fresh frame before enabling following')
        if self._state['phase'] in {'error','stopped'} or self._stop.is_set():
            raise RuntimeError('Application failed; stop and restart after fixing the reported error')
        if command in {'flip_pan','flip_tilt'}:
            # A new coordinate direction must never inherit an in-flight old pose.
            self.pause()
        with self._lock:
            try:
                self._commands.put_nowait((command,self._pause_revision))
            except queue.Full:
                raise RuntimeError('Another control is pending')
            if command=='resume':self._save_preference_locked(True,True)

    def _gestures_available(self) -> bool:
        # Follow can be off without disabling reflexes. Outages, stop-all and
        # temporary fist holds still block every ordinary gesture response.
        return (self._desired_gestures and self._armed and self._pause_reason in {'none','follow_only'}
                and (not self._pause.is_set() or self._pause_reason=='follow_only')
                and not self._stop.is_set() and not self._camera_fault.is_set()
                and not self._camera_stalled.is_set())

    def close(self) -> None:
        self.request_shutdown()
        if self._thread is not None:
            # No transport closure while a camera/servo call is still in flight.
            self._thread.join()

    def request_shutdown(self) -> None:
        self.pause(cause='shutdown')
        self._shutdown.set()
        self._stop.set()
        with self._image_ready:
            self._image_ready.notify_all()

    def request_social(self,name: str) -> bool:
        """Best effort cue; the existing single motor owner executes it."""
        if name!='nod':return False
        with self._lock:
            self._social_cue=(name,self._pause_revision,time.monotonic())
        return True

    def cancel_social(self) -> None:
        with self._lock:self._social_cue=None

    def _social_response(self) -> None:
        with self._lock:cue=self._social_cue;self._social_cue=None
        if cue is None:return
        name,revision,at=cue;now=time.monotonic()
        if (now-at>1.5 or now-self._last_social_cue<30 or revision!=self._revision()
            or self._pause.is_set() or not self._armed or self._stop.is_set()
            or self._camera_fault.is_set() or self._camera_stalled.is_set()
            or not self.social_response_allowed() or self._follow_move is not None):return
        state=self.snapshot()
        if (not state['pose_confirmed'] or state['tracking_status']!='centered'
            or state['frame_age_seconds'] is None or state['frame_age_seconds']>.7):return
        pan,tilt=state['pan_deg'],state['tilt_deg']
        # A small excursion with room inside the existing limits, then return.
        offset=-2 if tilt-2>=self.follower.tilt_limits[0] else 2
        if not self.follower.tilt_limits[0]<=tilt+offset<=self.follower.tilt_limits[1]:return
        self._last_social_cue=now
        try:
            self._move(pan,tilt+offset,300,revision=revision)
            self._move(pan,tilt,300,revision=revision)
            self._set_state(last_action='companion_nod')
        except InterruptedError:pass
        self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])

    def _set_state(self, **values) -> None:
        with self._lock:
            self._state.update(values)

    def _open_camera(self) -> None:
        if self._stop.is_set():
            raise InterruptedError('Application stopping')
        info = self.robot.refresh_device_info(timeout=5)
        self._set_state(device_info=info)
        if not self.robot.supports('camera.capture') or not self.robot.supports('motion'):
            raise RuntimeError('Firmware must expose camera.capture and motion')
        # The built-in follower must not compete with host motor corrections.
        if self.robot.supports('face_tracking.control.v1'):
            self.robot.face_tracking.stop(policy='hold', timeout=3)
        mode = self.config['camera_mode']
        if mode == 'inference':
            if not self.robot.supports('vision.inference.preview.v1'):
                self.event('Continuous preview unavailable; using robot JPEG snapshots')
                mode = 'capture'
            else:
                try:
                    self._session = self.robot.vision.start_inference(
                        self.config['vision_model_id'], preview=True, timeout=15)
                except (TimeoutError,WatcheRobotError):
                    # The SDK retains its own attempted session when a start ACK
                    # or its cleanup is lost. Its public stop retries that lease.
                    self._pending_inference_start=True
                    raise
        self._set_state(camera_mode=mode)
        self.gate.max_gap_seconds = 3.5 if mode == 'capture' else 1.5
        self.follower.max_gap_seconds = 3.5 if mode == 'capture' else 1.0
        # Known reference position, confirmed completion before relative corrections.
        if self.config['start_paused'] and not self._preference_saved:self._desired_follow=False
        if not self._desired_follow or self._restarting:
            self._pause.set()
            self._armed = False
            self._pause_reason='camera' if self._desired_follow or self._desired_gestures else 'user'
        else:
            try:self._move(90,115,800,interruptible=False)
            except InterruptedError:
                if self._stop.is_set():raise
                # A manual pause during reference motion keeps vision alive;
                # it must not terminate the camera worker before it starts.
                self.event('Startup reference paused; camera remains available')
        self.follower.reset()

    def _move(self, pan: float, tilt: float, duration: int, *, interruptible: bool = True,
              revision: int | None = None) -> None:
        revision = self._revision() if revision is None else revision
        # Discrete gestures/reference moves keep their sequencing. Only ordinary
        # following is non-blocking, and there is at most one in-flight motor job.
        while self._poll_follow_move():
            time.sleep(.01)
        def interrupted():
            return (self._stop.is_set() or self._camera_fault.is_set()
                    or revision != self._revision() or (interruptible and self._pause.is_set()))
        if interrupted():
            raise InterruptedError('Motion paused')
        pan = round(max(self.follower.pan_limits[0],min(self.follower.pan_limits[1],pan)))
        tilt = round(max(self.follower.tilt_limits[0],min(self.follower.tilt_limits[1],tilt)))
        pan_distance = abs(pan-self._state['pan_deg'])
        tilt_distance = abs(tilt-self._state['tilt_deg'])
        if not self._state['pose_confirmed']:
            pan_distance = max(abs(pan-x) for x in self.follower.pan_limits)
            tilt_distance = max(abs(tilt-x) for x in self.follower.tilt_limits)
        duration = max(duration,ceil(1000*max(pan_distance/20,tilt_distance/8)))
        # ACK failure does not prove the device never received the command.
        self._owns_motion = True
        self._set_state(pose_confirmed=False)
        job = self.robot.motion.move_to(pan_deg=pan,tilt_deg=tilt,duration_ms=duration,profile='linear')
        deadline = time.monotonic()+max(3,duration/1000+2)
        while not job.state.terminal:
            if interrupted():
                self.robot.motion.stop()
                self._set_state(pose_confirmed=False)
                raise InterruptedError('Motion paused')
            if time.monotonic() >= deadline:
                self.robot.motion.stop()
                raise TimeoutError('Servo motion did not finish')
            time.sleep(.05)
        if interrupted():
            self.robot.motion.stop()
            self._set_state(pose_confirmed=False)
            raise InterruptedError('Motion paused')
        job.wait(timeout=.1)
        self._motion_finished_at = time.monotonic()
        self._follow_ready_after = self._motion_finished_at+.05
        self._set_state(pan_deg=pan,tilt_deg=tilt,pose_confirmed=True)

    def _poll_follow_move(self) -> bool:
        pending = self._follow_move
        if pending is None:
            return False
        job,pan,tilt,started,deadline,revision = pending
        if (self._stop.is_set() or self._camera_fault.is_set() or (self._pause.is_set() and not self._interaction_move)
                or revision != self._revision()):
            self.robot.motion.stop()
            self._follow_move = None
            self._interaction_move = False
            self._paused_ack = self._pause.is_set()
            self._set_state(pose_confirmed=False,follow_active=False,follow_target=None)
            self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
            return False
        if not job.state.terminal:
            if time.monotonic() >= deadline:
                raise TimeoutError('Follow servo motion did not finish')
            return True
        job.wait(timeout=.1)
        interaction=self._interaction_move;kind=self._interaction_kind
        self._follow_move = None
        self._interaction_move = False
        self._motion_finished_at = time.monotonic()
        self._set_state(pan_deg=pan,tilt_deg=tilt,pose_confirmed=True,follow_active=False,follow_target=None,
                        follow_job_ms=round((self._motion_finished_at-started)*1000,1))
        if interaction:self.interaction_completed(kind,pan,tilt,self._motion_finished_at,revision)
        return False

    def _start_follow_move(self, pan: int, tilt: int, revision: int, *, interaction:bool=False,kind:str='sweep') -> None:
        if ((self._pause.is_set() and not interaction) or revision != self._revision() or self._stop.is_set()
                or self._camera_fault.is_set() or (not interaction and not self.follow_response_allowed())):
            return
        if not self._state['pose_confirmed']:
            raise RuntimeError('Follow reference is unknown; explicitly resume to recenter')
        pan=round(max(self.follower.pan_limits[0],min(self.follower.pan_limits[1],pan)))
        tilt=round(max(self.follower.tilt_limits[0],min(self.follower.tilt_limits[1],tilt)))
        duration = max(60,ceil(1000*max(abs(pan-self._state['pan_deg'])/self.follower.pan_speed,
                                      abs(tilt-self._state['tilt_deg'])/self.follower.tilt_speed)))
        started = time.monotonic()
        self._owns_motion = True
        self._set_state(pose_confirmed=False)
        job = self.robot.motion.move_to(pan_deg=pan,tilt_deg=tilt,duration_ms=duration,profile='linear')
        self._follow_move = (job,pan,tilt,started,started+duration/1000+3,revision)
        self._interaction_move=interaction
        self._interaction_kind=kind
        self._set_state(pose_confirmed=False,follow_active=True,follow_target=[pan,tilt],
                        follow_command_ms=round((time.monotonic()-started)*1000,1))
        with self._lock:
            self._state['servo_commands'] += 1
        if self._pause.is_set() or revision != self._revision() or self._camera_fault.is_set():
            self._poll_follow_move()

    def _visual(self, animation: str, color: str) -> None:
        if self.visual_feedback is not None:
            self.visual_feedback(animation,color)
        elif animation in self.robot.animation.available_ids:
            self._owns_animation = True
            self.robot.animation.play(animation)
        if self.robot.supports('light'):
            self._owns_light = True
            self.robot.lights.set_color(color,brightness=.25)

    def _resume(self, revision: int) -> None:
        if self._camera_stalled.is_set():raise InterruptedError('Waiting for a fresh camera frame')
        while self._poll_follow_move():
            time.sleep(.01)
        pan,tilt = self._state['pan_deg'],self._state['tilt_deg']
        if not self._state['pose_confirmed']:
            pan,tilt = 90,115
        self._move(pan,tilt,500,interruptible=False,revision=revision)
        with self._lock:
            if revision != self._pause_revision or self._stop.is_set():
                return
            self._pause.clear()
            self._armed = True
            self._pause_reason='none'
        self._paused_ack = False
        self.follower.reset(pan,tilt)

    def _gesture(self, name: str, *, revision: int | None = None) -> None:
        if name not in {'Closed_Fist','Pointing_Up'} and (not self.social_response_allowed() or not self.gesture_response_allowed(name)):
            return
        if name not in {'Closed_Fist','Pointing_Up'} and not self._gestures_available():return
        revision = self._revision() if revision is None else revision
        self.event('Gesture: '+name)
        with self._lock:
            self._state['gesture_events'] += 1
        if name == 'Closed_Fist':
            self._pause_for_fist()
            self.robot.motion.stop()
            self._paused_ack = True
            self.event('Following temporarily held; release hand to continue' if self._gesture_hold_active.is_set()
                       else 'Following paused; point upward to resume')
            return
        if name == 'Pointing_Up':
            with self._lock:
                if not self._armed or not self._pause.is_set() or revision!=self._pause_revision:return
                self._save_preference_locked(True)
                self._gesture_hold_active.clear();self._gesture_release_at=0.0
            self._resume(revision)
            self.event('Following resumed')
            return
        if not self._gestures_available():
            return
        while self._poll_follow_move():
            time.sleep(.01)
        if not self._gestures_available() or revision != self._revision():
            return
        pan,tilt = self._state['pan_deg'],self._state['tilt_deg']
        visuals = {'Open_Palm': ('smile','#35B7D8'), 'Thumb_Up': ('happy','#5CD087'),
                   'Thumb_Down': ('sad','#D2A45E'), 'Victory': ('sunglasses','#E7BA4C'),
                   'ILoveYou': ('fondle_love','#E677A0')}
        animation,color = visuals[name]
        self._visual(animation,color)
        # One owner: following is suspended while the response sequence executes.
        if name == 'Thumb_Up':
            poses = [(pan,tilt-5),(pan,tilt+4),(pan,tilt)]
        elif name == 'ILoveYou':
            poses = [(pan,tilt-3),(pan,tilt)]
        else:
            amplitude = 6 if name == 'Victory' else 8
            poses = [(pan-amplitude,tilt),(pan+amplitude,tilt),(pan,tilt)]
        self._set_state(last_action=name,phase='responding')
        try:
            for a,b in poses:
                self._move(a,b,400,revision=revision,interruptible=self._pause_reason!='follow_only')
        except InterruptedError:
            self.event('Response interrupted by pause')
        self.follower.reset(pan,tilt)

    def _control(self) -> None:
        self._poll_follow_move()
        if self._pause.is_set() and not self._paused_ack:
            self.robot.motion.stop()
            self._paused_ack = True
            self.event('Motion stopped')
        try:
            command,revision = self._commands.get_nowait()
        except queue.Empty:
            return
        if revision != self._revision():
            self.event('Older control discarded after pause: '+command)
            return
        try:
            if command == 'resume':
                self._resume(revision)
            elif command == 'recenter':
                self._move(90,115,800,interruptible=False,revision=revision)
                self.follower.reset()
            elif command == 'flip_pan':
                self.follower.pan_sign *= -1
                self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
            elif command == 'flip_tilt':
                self.follower.tilt_sign *= -1
                self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
            elif command == 'calibrate':
                self._calibrate(revision)
            if command in {'flip_pan','flip_tilt'}:
                self._save_axis_config()
            self.event('Control: '+command)
        except InterruptedError:
            self._pause.set()
            self.event('Control interrupted by pause: '+command)
            self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])

    def _save_axis_config(self) -> None:
        self.config.update(pan_sign=self.follower.pan_sign,tilt_sign=self.follower.tilt_sign)
        path = self.root/'config.json'
        temp = path.with_suffix('.json.tmp')
        temp.write_text(json.dumps(self.config,indent=2)+'\n',encoding='utf-8')
        temp.replace(path)

    def _calibration_frame(self, revision: int) -> bytes:
        # Calibration never opens a competing camera request. Wait for a frame
        # requested by the visual worker after motion and a short settling period.
        not_before = self._motion_finished_at+.25
        deadline = time.monotonic()+6
        with self._image_ready:
            while time.monotonic()<deadline:
                if self._stop.is_set() or revision != self._pause_revision:
                    raise InterruptedError('Calibration paused')
                if self._camera_fault.is_set():
                    raise RuntimeError(self._camera_error)
                if self._raw_jpeg is not None and self._raw_requested_at >= not_before:
                    return self._raw_jpeg
                self._image_ready.wait(timeout=.1)
        raise TimeoutError('No fresh camera frame for axis calibration')

    def _calibrate(self, revision: int | None = None) -> None:
        with self._lock:
            if (self._stop.is_set() or self._camera_fault.is_set()
                    or (revision is not None and revision != self._pause_revision)):
                raise InterruptedError('Calibration paused before startup')
            self._pause_revision += 1
            revision = self._pause_revision
            self._pause.set()
            self._armed = False
        self._set_state(phase='calibrating')
        self.event('Axis calibration: keep the background static')
        try:
            self._move(90,115,600,interruptible=False,revision=revision)
            before = self._calibration_frame(revision)
            self._move(97,115,600,interruptible=False,revision=revision)
            shifted = self._calibration_frame(revision)
            dx,_,pan_inliers = image_shift(before,shifted)
            self._move(90,115,600,interruptible=False,revision=revision)
            before = self._calibration_frame(revision)
            self._move(90,119,600,interruptible=False,revision=revision)
            shifted = self._calibration_frame(revision)
            _,dy,tilt_inliers = image_shift(before,shifted)
            pan_sign,tilt_sign = axis_sign(dx),axis_sign(dy)
            self.follower.pan_sign,self.follower.tilt_sign = pan_sign,tilt_sign
            self._save_axis_config()
            result = dict(pan_sign=pan_sign,tilt_sign=tilt_sign,dx=dx,dy=dy,
                          pan_inliers=pan_inliers,tilt_inliers=tilt_inliers)
            self._set_state(calibration=result)
            self.event('Axis calibration passed: '+json.dumps(result))
        except (ValueError,TimeoutError) as exc:
            self.event('Calibration inconclusive: '+str(exc))
            self._set_state(calibration={'error':str(exc)})
        finally:
            if not self._stop.is_set() and revision == self._revision():
                self._move(90,115,600,interruptible=False,revision=revision)
            self.follower.reset()
            self._paused_ack = True
            self.event('Calibration complete; motion paused')

    @staticmethod
    def _replace_latest(mailbox: queue.Queue, value) -> None:
        try:
            mailbox.put_nowait(value)
        except queue.Full:
            try:
                mailbox.get_nowait()
            except queue.Empty:
                pass
            mailbox.put_nowait(value)

    def _hold_for_camera(self) -> None:
        if not self._camera_stalled.is_set():
            self._camera_stalled.set()
            self.pause(cause='camera' if self._desired_follow or self._desired_gestures else 'user_hold')
            self._fresh_camera_frames=0
            self._gesture_release_at=0.0
            self._set_state(error='No new camera frame; motion held',phase='waiting_camera',
                            faces=0,gesture=None,score=0,tracking_status='camera_unavailable',target_center=None)
            self.event('Camera stalled; user visual preference retained; waiting for fresh frames')

    def _pause_for_fist(self) -> None:
        if self.config['continuous_follow'] and (self._desired_follow or self._desired_gestures):
            self._gesture_hold_active.set();self._gesture_release_at=0.0
            self.pause(disarm=False,cause='gesture')
        else:self.pause(disarm=False)

    def _observe_fist_release(self, observation, acquired_at:float, now:float) -> None:
        # Only new camera observations can release a temporary gesture stop.
        # Time passing during an outage is never evidence that a hand was lowered.
        if not self._gesture_hold_active.is_set():return
        if (now-acquired_at>.7 or observation.inference_ms>=800 or observation.gesture=='Closed_Fist'):
            self._gesture_release_at=0.0;return
        if not self._gesture_release_at:self._gesture_release_at=now
        elif now-self._gesture_release_at>=1.5:
            self._gesture_hold_active.clear();self._gesture_release_at=0.0

    def _restore_after_camera(self, acquired_at:float|None=None) -> None:
        with self._lock:
            fresh_at=self._frame_at if acquired_at is None else acquired_at
            if (not self._desired_follow and self._desired_gestures and self._pause_reason in {'camera','gesture'}
                and not self._gesture_hold_active.is_set()
                and not self._camera_stalled.is_set() and not self._camera_fault.is_set()
                and not self._stop.is_set() and not self._shutdown.is_set()
                and self._fresh_camera_frames>=3 and fresh_at and time.monotonic()-fresh_at<=.7):
                self._armed=True;self._pause_reason='follow_only'
                return  # No reference move and no implicit enabling of face following.
            if (not self._desired_follow or self._pause_reason not in {'camera','gesture'} or not self._pause.is_set()
                    or self._gesture_hold_active.is_set()
                    or self._camera_stalled.is_set() or self._camera_fault.is_set() or self._stop.is_set()
                    or self._shutdown.is_set() or self._fresh_camera_frames<3
                    or not fresh_at or time.monotonic()-fresh_at>.7):return
            revision=self._pause_revision
            self._state['phase']='restoring_visual'
        try:self._resume(revision)
        except InterruptedError:return
        if not self._pause.is_set():self.event('Fresh frames stable; following and gestures automatically restored')

    @staticmethod
    def _connection_failure(exc:Exception) -> bool:
        if isinstance(exc,(ConnectionError,ConnectionTimeoutError)):return True
        if isinstance(exc,(CommandError,JobError)):return exc.reason in {'not_connected','device_not_connected','disconnected','reconnect_timeout','connection_closed','transport_closed'}
        return isinstance(exc,WatcheRobotError) and any(x in str(exc).lower() for x in ('not connected','device_not_connected','transport disconnected','connection closed','reconnect_timeout'))

    def _recover_camera(self) -> None:
        """Release only our acknowledged session, then use plain robot JPEGs.

        The device inference task is not needed for host MediaPipe. Retrying
        result.get on the same wedged task cannot repair its owner/state.
        """
        now=time.monotonic()
        while self._camera_reopen_times and now-self._camera_reopen_times[0]>60:self._camera_reopen_times.popleft()
        if len(self._camera_reopen_times)>=2:raise CameraUnavailableError('Camera recovery limit reached; motion remains paused')
        self._camera_reopen_times.append(now)
        if self._session is not None:
            # Retain the lease if stop fails; never overlap a new owner.
            try:self._session.close(timeout=5)
            except Exception as exc:
                if isinstance(exc,TimeoutError) or self._connection_failure(exc):
                    raise CameraUnavailableError('Waiting for owned camera release') from exc
                raise
            self._session=None
        self._set_state(camera_mode='capture',camera_last_error='Inference stalled; using robot JPEG capture')
        self._inference_retry_after=time.monotonic()+15
        self.gate.max_gap_seconds=3.5;self.follower.max_gap_seconds=3.5
        with self._lock:self._state['camera_reopens']+=1
        self.event('Owned inference released; robot JPEG fallback; motion remains paused')

    def _restore_inference(self) -> bool:
        if (self._session is not None or not self._inference_retry_after
                or time.monotonic()<self._inference_retry_after or self._stop.is_set()):return False
        self._inference_retry_after=time.monotonic()+30
        self._hold_for_camera()
        try:
            self._pending_inference_start=True
            self._session=self.robot.vision.start_inference(self.config['vision_model_id'],preview=True,timeout=5)
            self._pending_inference_start=False
        except Exception as exc:
            try:self.robot.vision.stop_inference(timeout=4);self._pending_inference_start=False
            except Exception as cleanup:raise CameraUnavailableError('Waiting for owned preview retry cleanup') from cleanup
            if self._connection_failure(exc):raise CameraUnavailableError(str(exc)) from exc
            self._set_state(camera_last_error='Continuous preview retry deferred: '+type(exc).__name__)
            return False
        self._inference_retry_after=0
        self.gate.max_gap_seconds=1.5;self.follower.max_gap_seconds=1.0
        self._set_state(camera_mode='inference',camera_last_error=None)
        self.event('Owned continuous preview restored after JPEG fallback')
        return True

    def _camera_loop(self, perception: Perception) -> None:
        """Only camera/model calls run here. It never sends motor commands."""
        previous_sequence = None
        previous_frame_time = missing_since = epoch = time.monotonic()
        frame_count = 0
        consecutive_timeouts = 0
        try:
            while not self._stop.is_set():
                if self._restore_inference():previous_sequence=None
                requested_at = time.monotonic()
                # Retry only acquisition timeouts, never model/format errors.
                # This remains the sole camera thread; motor control stays held.
                try:
                    if self._session is not None:
                        result = self._session.latest(timeout=3)
                        if result is None or result.sequence == previous_sequence:
                            if time.monotonic()-missing_since > 5:self._hold_for_camera()
                            if time.monotonic()-missing_since > 20:
                                raise TimeoutError('No new robot camera frame for 20 seconds')
                            self._stop.wait(self.config['poll_interval'])
                            continue
                        previous_sequence = result.sequence
                        jpeg = result.jpeg
                    else:
                        jpeg = self.robot.camera.capture(width=640,height=480,timeout=5).data
                except (TimeoutError,ConnectionError,WatcheRobotError) as exc:
                    if self._connection_failure(exc):
                        self._hold_for_camera();raise CameraUnavailableError(str(exc)) from exc
                    if not isinstance(exc,(TimeoutError,CommandError)):raise
                    busy=isinstance(exc,CommandError)
                    capture_busy=(busy and self._session is None and exc.message_type=='ctrl.camera.capture'
                                  and exc.reason in {'busy','no_capacity'})
                    if busy and not capture_busy and (exc.message_type!='ctrl.vision.inference.result.get' or exc.reason!='busy'):raise
                    if self._stop.is_set():break
                    consecutive_timeouts += 1
                    self._hold_for_camera()
                    with self._lock:
                        self._state['camera_busy_retries' if busy else 'camera_timeouts'] += 1
                        self._state['camera_last_error'] = str(exc)[:200]
                        self._raw_jpeg = self._jpeg = None
                        self._frame_at = 0
                    if consecutive_timeouts>=3:
                        if capture_busy:
                            raise CameraUnavailableError('Robot JPEG capture has no capacity after 3 attempts; motion held; no other resource owner was stopped') from exc
                        self._recover_camera();previous_sequence=None
                        consecutive_timeouts=0;missing_since=time.monotonic()
                        self._stop.wait(.5);continue
                    self.event('Camera acquisition '+('busy' if busy else 'timeout')+'; retry '+str(consecutive_timeouts)+'/2; motion held')
                    self._stop.wait(.25*consecutive_timeouts)
                    continue
                acquired_at = time.monotonic()
                missing_since = acquired_at
                observation = perception.analyze(jpeg)
                if self._stop.is_set():
                    break
                now = time.monotonic()
                self._observe_fist_release(observation,acquired_at,now)
                frame_count += 1
                fps = 1/max(.001,now-previous_frame_time)
                previous_frame_time = now
                recovered = self._camera_stalled.is_set()
                self._camera_stalled.clear();consecutive_timeouts=0
                self._retryable_fault=False
                self._fresh_camera_frames=min(3,self._fresh_camera_frames+1) if observation.inference_ms<800 else 0
                if self._fresh_camera_frames>=3:self._recovery_delay=3.0
                if recovered:
                    with self._lock:self._state['camera_recoveries'] += 1
                    self.event('Fresh camera frame restored; waiting for stable frames and saved visual preference')
                with self._image_ready:
                    self._raw_jpeg = jpeg
                    self._raw_requested_at = requested_at
                    self._image_ready.notify_all()
                self._set_state(frames=frame_count,faces=len(observation.faces),
                                gesture=observation.gesture,score=round(observation.score,3),
                                fps=round(fps,1),average_fps=round(frame_count/max(1,now-epoch),1),
                                inference_ms=round(observation.inference_ms,1),error=None,
                                camera_request_ms=round((acquired_at-requested_at)*1000,1),
                                brightness=round(observation.brightness,1))
                name = observation.gesture
                if observation.inference_ms >= 800 and name != 'Closed_Fist':
                    name = None
                social_allowed=self.social_response_allowed()
                enabled=self._gestures_available() and social_allowed and self.gesture_response_allowed(name)
                # Disabled/busy observations must not consume a gesture latch.
                eligible=(self._desired_gestures and name in CONTROL_GESTURES) or enabled
                if not enabled:self.gate.reset_social()
                event = self.gate.update(name if eligible else None,observation.score if eligible else 0,now=now)
                if not self._desired_gestures:gesture_status='actions_disabled'
                elif not self._gestures_available():gesture_status='paused'
                elif not social_allowed:gesture_status='dialogue_busy'
                elif name is None:gesture_status='no_gesture'
                elif event:gesture_status='confirmed'
                else:gesture_status=self.gate.status(name)
                self._set_state(gesture_status=gesture_status,
                                gesture_threshold=GESTURE_THRESHOLDS.get(name,.65))
                if event == 'Closed_Fist':
                    # Stop intent reaches the motor wait loop even during a response.
                    self._pause_for_fist()
                    with self._lock:
                        self._state['gesture_events'] += 1
                        self._state['last_action'] = event
                    self.event('Gesture: Closed_Fist; temporary motion hold' if self._gesture_hold_active.is_set()
                               else 'Gesture: Closed_Fist; motion pause requested')
                elif event and self._armed and (enabled or event == 'Pointing_Up'):
                    self._replace_latest(self._gestures,(event,self._revision(),now))
                self._replace_latest(self._observations,(observation,acquired_at))
                if self.observation_listener is not None:
                    self.observation_listener(observation,jpeg,acquired_at)
                state = self.snapshot()
                cv2.putText(observation.frame,
                            f'{state["phase"]} pan={state["pan_deg"]} tilt={state["tilt_deg"]} fps={fps:.1f}',
                            (12,observation.frame.shape[0]-12),cv2.FONT_HERSHEY_SIMPLEX,.5,(230,230,230),1)
                ok,encoded = cv2.imencode('.jpg',observation.frame,[cv2.IMWRITE_JPEG_QUALITY,80])
                if ok:
                    with self._lock:
                        self._jpeg = encoded.tobytes()
                        self._frame_at = acquired_at
                self._stop.wait(self.config['poll_interval'])
        except Exception as exc:
            self._retryable_fault=isinstance(exc,CameraUnavailableError) or self._connection_failure(exc)
            self._camera_error = f'{type(exc).__name__}: {exc}'
            self._camera_fault.set()
            self.pause(cause='camera' if self._retryable_fault else 'fault')
            with self._image_ready:
                self._image_ready.notify_all()

    def _follow(self, observation, acquired_at: float) -> None:
        if not self.follow_response_allowed():
            if (self._stop.is_set() or self._camera_fault.is_set() or self._camera_stalled.is_set()
                or time.monotonic()-acquired_at>.7 or observation.inference_ms>=800):return
            if self._poll_follow_move():return
            directive=self.interaction_search(self.snapshot(),observation,acquired_at)
            self._set_state(tracking_status='interaction_search' if directive else 'interaction_hold')
            if directive and directive['kind']=='center':
                if not self._search_centering:self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
                self._search_centering=True
            else:
                self._search_centering=False
                self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
            if directive and directive['revision']==self._revision():
                if directive['kind']=='reference':
                    try:self._move(90,115,800,interruptible=False,revision=directive['revision'])
                    except InterruptedError:pass
                    self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
                else:
                    target=directive.get('target')
                    if directive['kind']=='center':target=self.follower.update(observation.faces,now=acquired_at)
                    if target is not None:
                        a,b=target;pan,tilt=self._state['pan_deg'],self._state['tilt_deg']
                        self._start_follow_move(pan+max(-6,min(6,a-pan)),tilt+max(-2,min(2,b-tilt)),
                                                directive['revision'],interaction=True,kind=directive['kind'])
            return
        if (self._pause.is_set() or observation.inference_ms >= 800
                or acquired_at <= self._follow_ready_after
                or time.monotonic()-acquired_at > 1):
            if self._pause.is_set():
                self.follower.reset(self._state['pan_deg'],self._state['tilt_deg'])
            return
        if self._poll_follow_move():
            return
        revision = self._revision()
        target = self.follower.update(observation.faces,now=acquired_at)
        if target is not None:
            self._set_state(control_frame_age_ms=round((time.monotonic()-acquired_at)*1000,1))
            self._start_follow_move(*target,revision)
        center = self.follower.target.center if self.follower.target_visible else None
        centered = center is not None and abs(center[0]-.5)<=.06 and abs(center[1]-.5)<=.07
        status = ('no_face' if not observation.faces else 'target_lost' if center is None
                  else 'centered' if centered else 'tracking')
        if center is not None:
            horizontal_error = self.follower.pan_sign*(center[0]-.5)
            vertical_error = self.follower.tilt_sign*(center[1]-.5)
            pan_limited = abs(center[0]-.5)>.06 and (
                (self.follower.pan<=self.follower.pan_limits[0] and horizontal_error<0)
                or (self.follower.pan>=self.follower.pan_limits[1] and horizontal_error>0))
            tilt_limited = abs(center[1]-.5)>.07 and (
                (self.follower.tilt<=self.follower.tilt_limits[0] and vertical_error<0)
                or (self.follower.tilt>=self.follower.tilt_limits[1] and vertical_error>0))
            if pan_limited or tilt_limited:
                status = 'joint_limit'
        self._set_state(tracking_status=status,target_center=center)

    def _release_owned(self) -> bool:
        released=True
        if self._owns_motion:
            try:self.robot.motion.stop();self._owns_motion=False
            except Exception as exc:self.event('Motion cleanup pending: '+str(exc));released=False
        self._follow_move=None;self._interaction_move=False;self._set_state(follow_active=False,follow_target=None)
        if self._camera_thread is not None:self._camera_thread.join()
        if self._session is not None:
            try:self._session.close(timeout=4);self._session=None
            except Exception as exc:self.event('Camera cleanup pending: '+str(exc));released=False
        if self._pending_inference_start:
            try:self.robot.vision.stop_inference(timeout=4);self._pending_inference_start=False
            except Exception as exc:self.event('Camera start cleanup pending: '+str(exc));released=False
        for flag,target,method in (('_owns_animation','animation','stop'),('_owns_light','lights','off')):
            if getattr(self,flag):
                try:getattr(getattr(self.robot,target),method)();setattr(self,flag,False)
                except Exception as exc:self.event('Response cleanup pending: '+str(exc));released=False
        return released

    def _run(self) -> None:
        while not self._shutdown.is_set():
            self._retryable_fault=False
            self._run_once()
            if self._shutdown.is_set() or not self._retryable_fault:return
            release_pending=bool(self._session is not None or self._pending_inference_start
                                 or self._owns_motion or self._owns_animation or self._owns_light)
            self._set_state(phase='waiting_release' if release_pending else 'waiting_connection',
                            tracking_status='camera_unavailable',pose_confirmed=False)
            while not self._shutdown.wait(self._recovery_delay):
                self._recovery_attempts+=1
                self._recovery_delay=min(30,self._recovery_delay*2)
                if not self._release_owned():
                    self._set_state(phase='waiting_release',error='等待自己持有的资源确认释放，再恢复视觉');continue
                try:self.robot.refresh_device_info(timeout=3)
                except Exception as exc:
                    self._set_state(phase='waiting_connection',error='等待原机器人连接恢复：'+type(exc).__name__);continue
                # Previous owner/camera threads have fully ended and leases have
                # acknowledged closure. Only now may this same owner start again.
                self._restarting=True;self._fresh_camera_frames=0
                self._gesture_release_at=0.0
                self._camera_error=None;self._camera_fault.clear();self._camera_stalled.clear()
                self._stop.clear();self._frame_at=0;self._raw_jpeg=self._jpeg=None
                for mailbox in (self._commands,self._gestures,self._observations):
                    while True:
                        try:mailbox.get_nowait()
                        except queue.Empty:break
                self.gate=GestureGate(hold_seconds=self.config['gesture_hold_seconds'],cooldown=self.config['gesture_cooldown_seconds'],continuation_thresholds={'Thumb_Up':.5})
                self._set_state(phase='starting',error=None)
                break
            else:return

    def _run_once(self) -> None:
        perception = None
        hardware_ready=False
        opening_camera=False
        try:
            perception = Perception(self.root/'models')
            hardware_ready=True
            opening_camera=True
            self._open_camera()
            opening_camera=False
            self.event('Local MediaPipe gesture and face models ready')
            self._camera_thread = threading.Thread(target=self._camera_loop,args=(perception,),
                                                   name='robot-camera-models',daemon=False)
            self._camera_thread.start()
            while not self._stop.is_set():
                if self._camera_fault.is_set():
                    raise RuntimeError(self._camera_error)
                self._control()
                self._social_response()
                try:
                    name,revision,detected_at = self._gestures.get_nowait()
                except queue.Empty:
                    pass
                else:
                    if revision == self._revision() and time.monotonic()-detected_at <= 1.5:
                        try:
                            self._gesture(name,revision=revision)
                        except InterruptedError:
                            self.event('Gesture control interrupted by pause')
                try:
                    observation,acquired_at = self._observations.get(timeout=.05)
                except queue.Empty:
                    continue
                self._restore_after_camera(acquired_at)
                self._follow(observation,acquired_at)
                self._set_state(phase='waiting_camera' if self._camera_stalled.is_set() else
                                'paused' if self._pause.is_set() else 'running')
                if self._pause.is_set():
                    self._set_state(tracking_status='paused')
        except InterruptedError:
            self._set_state(phase='stopped')
        except Exception as exc:
            if self._connection_failure(exc) or (hardware_ready and isinstance(exc,TimeoutError)):self._retryable_fault=True
            if (opening_camera and isinstance(exc,CommandError) and exc.message_type=='ctrl.vision.inference.start'
                    and exc.reason in {'busy','no_capacity'}):self._retryable_fault=True
            self.pause(cause='camera' if self._retryable_fault else 'fault')
            self._set_state(phase='error',error=f'{type(exc).__name__}: {exc}')
            self.logger.exception('Vision application stopped on error')
        finally:
            self._stop.set()
            with self._image_ready:
                self._image_ready.notify_all()
            self._release_owned()
            if perception is not None:
                perception.close()


def create_web_app(service: VisionService) -> FastAPI:
    app = FastAPI(title='Robot vision controls')

    @app.get('/',response_class=HTMLResponse)
    def page():
        return (service.root/'preview.html').read_text(encoding='utf-8')

    @app.get('/api/status')
    def status():
        return service.snapshot()

    @app.get('/frame.jpg')
    def frame():
        jpeg = service.jpeg()
        if jpeg is None:
            raise HTTPException(503,'Waiting for robot camera')
        return Response(jpeg,media_type='image/jpeg',headers={'Cache-Control':'no-store'})

    @app.get('/stream.mjpg')
    def stream():
        async def frames():
            previous = None
            while not service._shutdown.is_set():
                jpeg = service.jpeg()
                if jpeg is not None and jpeg is not previous:
                    previous = jpeg
                    yield b'--frame\r\nContent-Type: image/jpeg\r\n\r\n'+jpeg+b'\r\n'
                await asyncio.sleep(.1)
        return StreamingResponse(frames(),media_type='multipart/x-mixed-replace; boundary=frame')

    @app.post('/api/control/{command}')
    async def control(command: str,request: Request):
        origin = request.headers.get('origin')
        if origin is not None and origin != f'http://127.0.0.1:{service.config["port"]}':
            raise HTTPException(403,'Cross-origin control rejected')
        try:
            service.request(command)
        except ValueError as exc:
            raise HTTPException(400,str(exc)) from exc
        except RuntimeError as exc:
            raise HTTPException(409,str(exc)) from exc
        return {'accepted':True,'command':command}

    return app
