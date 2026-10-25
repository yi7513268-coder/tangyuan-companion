"""Pure gesture gating and face-following policy; all coordinates are normalized."""

from __future__ import annotations

from dataclasses import dataclass
from math import hypot, isfinite

GESTURES = frozenset({'Open_Palm', 'Closed_Fist', 'Thumb_Up', 'Thumb_Down',
                      'Victory', 'Pointing_Up', 'ILoveYou'})
CONTROL_GESTURES = frozenset({'Closed_Fist', 'Pointing_Up'})
GESTURE_THRESHOLDS = {'Open_Palm': .55}


def choose_gesture(candidates: list[tuple[str, float]]) -> tuple[str | None, float]:
    valid = [(name, score) for name, score in candidates
             if name in GESTURES and isfinite(score) and 0 <= score <= 1]
    for control in ('Closed_Fist', 'Pointing_Up'):
        matches = [(name, score) for name, score in valid if name == control and score >= .65]
        if matches:
            return max(matches, key=lambda item: item[1])
    return max(valid, key=lambda item: item[1]) if valid else (None, 0.0)


@dataclass(frozen=True)
class Face:
    x: float
    y: float
    width: float
    height: float

    @property
    def center(self) -> tuple[float, float]:
        return self.x + self.width / 2, self.y + self.height / 2


class FaceFollower:
    """Bounded velocity controller with deadband, smoothing and target continuity."""

    def __init__(self, *, pan_sign: int = -1, tilt_sign: int = 1,
                 pan_limits: tuple[int, int] = (30, 150),
                 tilt_limits: tuple[int, int] = (100, 130),
                 max_gap_seconds: float = 1.0, pan_speed: float = 36,
                 tilt_speed: float = 16, smoothing: float = .75) -> None:
        if pan_sign not in (-1, 1) or tilt_sign not in (-1, 1):
            raise ValueError('axis signs must be -1 or 1')
        self.pan_sign, self.tilt_sign = pan_sign, tilt_sign
        self.pan_limits, self.tilt_limits = pan_limits, tilt_limits
        self.max_gap_seconds = max_gap_seconds
        self.pan_speed, self.tilt_speed, self.smoothing = pan_speed, tilt_speed, smoothing
        self.pan, self.tilt = 90.0, 115.0
        self.target: Face | None = None
        self.target_visible = False
        self._last_seen = -float('inf')
        self._last_time: float | None = None
        self._error = (0.0, 0.0)
        self._last_sent = (90, 115)

    def reset(self, pan: float = 90, tilt: float = 115) -> None:
        self.pan = max(self.pan_limits[0], min(self.pan_limits[1], pan))
        self.tilt = max(self.tilt_limits[0], min(self.tilt_limits[1], tilt))
        self.target = None
        self.target_visible = False
        self._last_time = None
        self._last_seen = -float('inf')
        self._error = (0.0, 0.0)
        self._last_sent = (round(self.pan), round(self.tilt))

    def update(self, faces: list[Face], *, now: float) -> tuple[int, int] | None:
        valid = [f for f in faces if f.width > 0 and f.height > 0
                 and all(isfinite(v) for v in (f.x, f.y, f.width, f.height))]
        target = None
        if valid:
            if self.target is not None and now - self._last_seen < 1.2:
                cx, cy = self.target.center
                candidate = min(valid, key=lambda f: hypot(f.center[0]-cx, f.center[1]-cy))
                if hypot(candidate.center[0]-cx, candidate.center[1]-cy) < .28:
                    target = candidate
            else:
                target = max(valid, key=lambda f: f.width * f.height)
        if target is None:
            self.target_visible = False
            self._last_time = None
            self._error = (0.0, 0.0)
            return None
        self.target = target
        self.target_visible = True
        self._last_seen = now
        previous = self._last_time
        self._last_time = now
        if previous is None or now - previous > self.max_gap_seconds:
            self._error = (0.0, 0.0)
            return None
        dt = max(0.0, min(.25, now-previous))
        cx, cy = target.center
        raw = (2 * (cx-.5), 2 * (cy-.5))
        self._error = tuple(self.smoothing*e + (1-self.smoothing)*p for e,p in zip(raw,self._error))
        ex, ey = self._error
        # No integration inside the dead zone; bounded configurable velocity.
        vx = 0.0 if abs(ex) < .12 else max(-self.pan_speed,min(self.pan_speed,2*self.pan_speed*ex))
        vy = 0.0 if abs(ey) < .14 else max(-self.tilt_speed,min(self.tilt_speed,2*self.tilt_speed*ey))
        self.pan = max(self.pan_limits[0], min(self.pan_limits[1], self.pan + self.pan_sign*vx*dt))
        self.tilt = max(self.tilt_limits[0], min(self.tilt_limits[1], self.tilt + self.tilt_sign*vy*dt))
        angles = round(self.pan), round(self.tilt)
        if angles == self._last_sent:
            return None
        self._last_sent = angles
        return angles


class GestureGate:
    """One trigger per stable gesture, requiring release before repeating it."""

    def __init__(self, *, hold_seconds: float = .5, cooldown: float = 2.5,
                 max_gap_seconds: float = 1.5, continuation_thresholds: dict[str,float] | None = None) -> None:
        self.hold_seconds, self.cooldown = hold_seconds, cooldown
        self.max_gap_seconds = max_gap_seconds
        self._candidate: str | None = None
        self._since = 0.0
        self._last_time: float | None = None
        self._latched: str | None = None
        self._release_since: float | None = None
        self._last_trigger = -float('inf')
        self._samples = 0
        self.continuation_thresholds = dict(continuation_thresholds or {})
        if any(name in CONTROL_GESTURES or name not in GESTURES or not isfinite(value)
               or not 0 < value < GESTURE_THRESHOLDS.get(name,.65)
               for name,value in self.continuation_thresholds.items()):
            raise ValueError('Continuation thresholds are only for social gestures')
        self._strong_at = self._supported_at = -float('inf')

    def reset_social(self) -> None:
        """Disabled interaction must not consume a social gesture's latch."""
        if self._candidate not in CONTROL_GESTURES:
            self._candidate=None;self._samples=0
            self._strong_at=self._supported_at=-float('inf')
        if self._latched not in CONTROL_GESTURES:self._latched=None

    def status(self, gesture: str) -> str:
        if self._latched==gesture:return 'release_required'
        return 'confirming' if self._candidate==gesture else 'low_confidence'

    def update(self, gesture: str | None, score: float, *, now: float) -> str | None:
        raw_gesture = gesture
        threshold = GESTURE_THRESHOLDS.get(gesture,.65)
        strong = gesture in GESTURES and isfinite(score) and threshold <= score <= 1
        continuing = (gesture == self._candidate and gesture in self.continuation_thresholds
                      and now-self._strong_at <= 1 and now-self._supported_at <= .65
                      and isfinite(score) and self.continuation_thresholds[gesture] <= score <= 1)
        if not strong and not continuing:
            gesture = None
        if self._last_time is not None and now-self._last_time > self.max_gap_seconds:
            self._candidate = None
            self._samples = 0
            self._release_since = None
            if not strong:gesture=None
        self._last_time = now
        if gesture is None:
            # One brief missing frame may bridge an already strong candidate.
            # It never adds a sample or triggers a motion by itself.
            if (self._candidate in self.continuation_thresholds and raw_gesture in {None,self._candidate}
                and self._latched != self._candidate
                and now-self._supported_at <= .35 and now-self._strong_at <= 1):
                return None
            self._candidate = None
            self._samples = 0
            if self._release_since is None:
                self._release_since = now
            elif now-self._release_since >= .4:
                self._latched = None
            return None
        if strong:self._strong_at = now
        self._supported_at = now
        self._release_since = None
        if gesture != self._candidate:
            self._candidate, self._since, self._samples = gesture, now, 1
            return None
        self._samples += 1
        minimum_samples = 3 if gesture in self.continuation_thresholds else 2
        if (self._samples >= minimum_samples and now-self._since >= self.hold_seconds
                and (gesture in CONTROL_GESTURES or now-self._last_trigger >= self.cooldown)
                and gesture != self._latched):
            self._latched, self._last_trigger = gesture, now
            return gesture
        return None
