# Prints the launch counter's numbers (see worker.js).
#   python -I stats_report.py
# Reads two private files outside the repo: %USERPROFILE%\.bwvr\stats_url.txt (the Worker's address,
# e.g. https://bwvr-stats.<name>.workers.dev) and %USERPROFILE%\.bwvr\stats_key.txt (its STATS_KEY).
import json, os, urllib.request

home = os.path.join(os.path.expanduser('~'), '.bwvr')
url = open(os.path.join(home, 'stats_url.txt'), encoding='utf-8').read().strip().rstrip('/')
key = open(os.path.join(home, 'stats_key.txt'), encoding='utf-8').read().strip()
req = urllib.request.Request(url + '/stats', headers={'Authorization': 'Bearer ' + key, 'User-Agent': 'bwvr-stats'})
with urllib.request.urlopen(req) as r: s = json.load(r)

t = s['total']
print('All time: %s players, %s launches (since %s)' % (t['players'] or 0, t['launches'] or 0, t['since'] or '-'))
print('\nPlayers on each version, last 7 days:')
for v in s['versions_7d']: print('  %-8s %d' % (v['version'], v['players']))
fresh = {d['day']: d['new_players'] for d in s['new_players']}
print('\nLast 30 days:   players  new  launches')
for d in s['days']:
    print('  %s  %7d  %3d  %8d' % (d['day'], d['players'], fresh.get(d['day'], 0), d['launches']))
