"""Pure music matching; unclear requests return choices instead of arbitrary audio."""
import re
import unicodedata
from difflib import SequenceMatcher

def normalize(text):
    text = unicodedata.normalize('NFKD', text.lower().replace('đ', 'd'))
    return ' '.join(re.sub(r'[^a-z0-9]+', ' ', ''.join(c for c in text if not unicodedata.combining(c))).split())

def score_candidate(query, artist, item):
    q = normalize(query)
    for prefix in ('mo bai hat ', 'phat bai hat ', 'nghe bai hat ', 'mo nhac ', 'mo ', 'phat '):
        if q.startswith(prefix):
            q = q[len(prefix):]
            break
    if ' cua ' in q and not artist:
        q, artist = q.split(' cua ', 1)
    title = normalize(item.get('title') or '')
    channel = normalize(item.get('artist') or item.get('uploader') or item.get('channel') or '')
    tokens = set(q.split())
    if not tokens:
        return 0.0
    recall = len(tokens & set((title + ' ' + channel).split())) / len(tokens)
    exact = q in title
    artist_tokens = set(normalize(artist).split())
    artist_match = len(artist_tokens & set((title + ' ' + channel).split())) / len(artist_tokens) if artist_tokens else recall
    source = 1.0 if any(x in channel for x in ('official', 'topic')) else 0.5
    score = .65 * recall + .25 * artist_match + .10 * source
    if exact:
        score = min(1.0, score + .05)
    for variant in ('cover', 'remix', 'karaoke', 'live', 'reaction', 'review', 'softer', 'speed', 'slowed'):
        if variant in title.split() and variant not in q.split():
            score -= .24
    for phrase in ('kien thuc', 'su that thu vi', 'podcast', 'tin tuc'):
        if phrase in title and phrase not in q:
            score -= .6
    duration = item.get('duration') or 0
    if item.get('is_live') or duration > 1200:
        score -= .35
    return round(score, 4)

def choose_candidate(query, artist, items):
    ranked = sorted(({**item, 'match_score': score_candidate(query, artist, item)} for item in items),
                    key=lambda item: item['match_score'], reverse=True)
    if not ranked or ranked[0]['match_score'] < .72:
        return None, ranked[:3]
    if len(ranked) > 1 and ranked[0]['match_score'] - ranked[1]['match_score'] < .08:
        # Duplicate uploads of a near-identical title are interchangeable only when the channel matches.
        same_artist = normalize(ranked[0].get('artist', '')) == normalize(ranked[1].get('artist', ''))
        similar = SequenceMatcher(None, normalize(ranked[0]['title']), normalize(ranked[1]['title'])).ratio() > .9
        if not (same_artist and similar):
            return None, ranked[:3]
    return ranked[0], ranked[:3]

class AmbiguousMusic(ValueError):
    def __init__(self, choices):
        super().__init__('Music selection needs clarification')
        self.choices = [{k: row.get(k) for k in ('title', 'artist', 'duration_str', 'url')} for row in choices]
