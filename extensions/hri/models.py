"""Transparent research adaptations; never label these as author checkpoints.

DBN uses supervised Bernoulli emissions and a two-slice latent engagement node.
Missing modalities are marginalized. No emotion, identity or intent is inferred.
CACAS-style Q/feedback combination is an independent architectural experiment;
the author's simulator, reward, policy weights and hyperparameters are unavailable.
"""
import json
import math
import random
from pathlib import Path

FEATURES = ('head_facing', 'body_facing', 'recent_reply')
ACTIONS = ('wait', 'screen', 'speak')


class EngagementDBN:
    def __init__(self, parameters=None):
        self.parameters = parameters
        self.posterior = [.5, .5]
        self.last_sequence = None

    @staticmethod
    def fit(rows):
        # Labels must be supplied by a person, grouped by session. Never infer
        # labels from the same features whose classifier is being evaluated.
        if len(rows) < 12 or {r['label'] for r in rows} != {0, 1}:
            raise ValueError('Need at least 12 explicitly labelled rows and both labels')
        transition = [[1., 1.], [1., 1.]]
        emissions = [[[1., 1.] for _ in FEATURES] for _ in range(2)]
        previous = {}
        for r in rows:
            y = r['label']
            if type(y) is not int or y not in (0, 1):
                raise ValueError('Binary explicit labels required')
            key = r['session_id']
            last = previous.get(key)
            if last and 0 < r['at'] - last['at'] <= 2:
                transition[last['label']][y] += 1
            previous[key] = r
            for i, name in enumerate(FEATURES):
                x = r.get(name)
                if x is not None:
                    if type(x) is not bool: raise ValueError('Boolean or missing features required')
                    emissions[y][i][int(x)] += 1
        return dict(method='two_slice_supervised_DBN_adaptation',author_parameters=False,
                    training_sessions=sorted({r['session_id'] for r in rows}),
                    transition=[[v/sum(row) for v in row] for row in transition],
                    emission=[[pair[1]/sum(pair) for pair in row] for row in emissions])

    def observe(self, sequence, values, fresh=True):
        if not fresh:
            self.posterior = [.5, .5]; self.last_sequence = None
            return dict(status='unknown',probability=None,reason='stale_or_missing_frame')
        if self.parameters is None:
            return dict(status='awaiting_calibration',probability=None,calibrated=False)
        if sequence == self.last_sequence:
            return dict(status='duplicate',probability=round(self.posterior[1],5))
        self.last_sequence = sequence
        predicted = [sum(self.posterior[i]*self.parameters['transition'][i][j]
                         for i in range(2)) for j in range(2)]
        seen = 0
        for n, name in enumerate(FEATURES):
            x = values.get(name)
            if x is None: continue
            if type(x) is not bool: raise ValueError('Invalid feature')
            seen += 1
            for state in range(2):
                p = self.parameters['emission'][state][n]
                predicted[state] *= p if x else 1-p
        total = sum(predicted)
        self.posterior = [v/total for v in predicted] if total else [.5, .5]
        return dict(status='estimated' if seen else 'prediction_only',
                    probability=round(self.posterior[1],5),observed_modalities=seen,
                    author_model=False,calibration_domain='local_labelled_sessions')


class BalancedController:
    """RO-MAN 2026 System 2 orientation-edge component, desktop adaptation.

    No mobile base, metric distance or hospital task locations on this platform.
    A pending real result is needed; no fabricated reminders are generated.
    """
    def __init__(self):
        self.facing_since=None; self.away_since=None; self.armed=False
        self.last_emit=None; self.last_now=None

    def decide(self, *, now, facing, fresh, pending, busy, speaking, quiet, explicit=False):
        if self.last_now is not None and now < self.last_now: raise ValueError('Nonmonotonic clock')
        self.last_now=now
        if quiet: self.armed=False; return 'wait','quiet'
        if busy or speaking: return 'wait','explicit_busy_or_speech'
        if explicit and pending: self.armed=False; return 'speak','explicit_request'
        if not fresh or facing is None:
            self.facing_since=None; self.away_since=None; self.armed=False
            return 'wait','unknown_visual_state'
        if not facing:
            self.facing_since=None
            if self.away_since is None:self.away_since=now
            if now-self.away_since>=2:self.armed=True
            return 'wait','facing_away'
        self.away_since=None
        if self.facing_since is None:self.facing_since=now
        if self.armed and pending and now-self.facing_since>=1.5 and (self.last_emit is None or now-self.last_emit>=60):
            self.armed=False; self.last_emit=now
            return 'speak','returned_attention_with_pending_result'
        return 'wait','no_new_attention_transition'


class AdaptiveConversation:
    """Q-learning + supervised explicit preference evidence. Shadow only.

    Synthetic simulator is deliberately separate from real feedback. Its scores
    are NOT measurements of benefit to the user or a CACAS paper reproduction.
    """
    def __init__(self, q=None):
        self.q=q or {}; self.labels={}; self.pending={}

    def train_simulator(self, seed=20261023, episodes=2000):
        rng=random.Random(seed); states=('absent','busy','available')
        # Engineering assumptions, not author rewards or human preferences.
        rewards={'absent':(0,-1,-3),'busy':(0,-.2,-3),'available':(-.3,.2,1)}
        for _ in range(episodes):
            state=rng.choice(states); action=rng.randrange(3)
            q=self.q.setdefault(state,[0.,0.,0.]); nxt=rng.choice(states)
            target=rewards[state][action]+.8*max(self.q.get(nxt,[0.,0.,0.]))
            q[action]+=.1*(target-q[action])
        return dict(q=self.q,training_domain='synthetic_engineering_simulator',
                    seed=seed,episodes=episodes,author_reproduction=False,live_enabled=False)

    def proposal(self, state):
        if state not in self.q:return dict(action='wait',reason='unseen_context',live_enabled=False)
        q=self.q[state]; spread=max(q)-min(q) or 1
        counts=self.labels.get(state,[[1,1] for _ in ACTIONS])
        # Mix learned RL utility and explicitly supervised accept/reject evidence.
        scores=[.5*(v-min(q))/spread+.5*(pair[0]/sum(pair)) for v,pair in zip(q,counts)]
        index=max(range(3),key=lambda i:scores[i])
        return dict(action=ACTIONS[index],scores=scores,live_enabled=False,
                    method='independent_Q_plus_supervised_adaptation_not_CACAS_reproduction')

    def completed(self, identifier, state, action, now):
        if identifier in self.pending:raise ValueError('Duplicate interaction')
        if action not in ACTIONS:raise ValueError('Invalid action')
        self.pending[identifier]=dict(state=state,action=action,at=now,label=None)

    def feedback(self, identifier, accepted, now):
        item=self.pending.get(identifier)
        if not item or not 0<=now-item['at']<=600 or item['label'] is not None or type(accepted) is not bool:
            raise ValueError('Need unique recent explicitly labelled completed interaction')
        item['label']=accepted
        pairs=self.labels.setdefault(item['state'],[[1,1] for _ in ACTIONS])
        pairs[ACTIONS.index(item['action'])][0 if accepted else 1]+=1
        return dict(updated=True,live_enabled=False)


def evaluate_sessions(rows):
    sessions=sorted({r['session_id'] for r in rows})
    if len(sessions)<3:raise ValueError('Need at least three independent labelled sessions')
    predictions=[]
    for held in sessions:
        train=[r for r in rows if r['session_id']!=held]
        model=EngagementDBN(EngagementDBN.fit(train))
        for i,r in enumerate(r for r in rows if r['session_id']==held):
            out=model.observe(i,r); predictions.append((r['label'],out['probability']))
    n=len(predictions);accuracy=sum(y==int(p>=.5) for y,p in predictions)/n
    brier=sum((p-y)**2 for y,p in predictions)/n
    return dict(protocol='leave_one_session_out',sessions=len(sessions),samples=n,
                accuracy=accuracy,brier=brier,physical_hri_benefit_verified=False)
