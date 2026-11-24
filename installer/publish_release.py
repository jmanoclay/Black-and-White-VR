# Creates or updates a GitHub release and uploads its assets, with git's stored github.com sign-in.
#   python -I publish_release.py <owner/repo> <tag> <title> <notes.md> <asset>... [--remove <asset name>]...
#                                [--no-discord] [--discord-preview]
# If the release exists, its title and notes are replaced, an asset of the same name is replaced,
# and --remove deletes an old asset. Publishing the installer (JakeypooBWVRModInstaller.exe and
# installer.json) to a release takes it off every other release: one installer, the newest.
# A NEW release is also announced in the Discord #releases channel (a card with the highlights of the
# notes and the download link), through the webhook URL in a private file outside the repo:
# %USERPROFILE%\.bwvr\discord_webhook.txt (or the file named by BWVR_DISCORD_WEBHOOK_FILE). No file,
# or --no-discord: no post. --discord-preview prints the card and stops, touching nothing.
import json, os, re, subprocess, sys, urllib.request, urllib.error

args = sys.argv[1:]
remove = [args[i + 1] for i, a in enumerate(args) if a == '--remove']
args = [a for i, a in enumerate(args) if a != '--remove' and (i == 0 or args[i - 1] != '--remove')]
no_discord = '--no-discord' in args
preview = '--discord-preview' in args
args = [a for a in args if a not in ('--no-discord', '--discord-preview')]
repo, tag, title, notes_file = args[:4]
assets = args[4:]

GOLD = (212 << 16) | (175 << 8) | 92  # the installer's gold
INSTALLER_URL = 'https://github.com/%s/releases/latest/download/JakeypooBWVRModInstaller.exe' % repo

def shorten(bullet, limit=150):
    # "**Name** rest...": the bold name kept, the rest plain and cut to its first sentence or the limit.
    m = re.match(r'\*\*(.+?)\*\*(.*)', bullet)
    head, rest = ('**%s**' % m.group(1), m.group(2)) if m else ('', bullet)
    rest = rest.replace('**', '')
    end = rest.find('. ')
    if end >= 0: rest = rest[:end + 1]
    if len(head) + len(rest) > limit:
        cut = rest[:max(0, limit - len(head))].rsplit(' ', 1)[0].rstrip(',;:(')
        rest = cut + '…'
    return head + rest

def discord_card(notes, page_url):
    # The highlights: each section's bullets, shortened; not the known issues or the installer's own notes.
    lines, section, skip, count = [], None, False, 0
    for raw in notes.replace('\r', '').split('\n'):
        t = raw.strip()
        if t.startswith('#'):
            section = t.lstrip('#').strip()
            skip = section.lower().startswith(('known issues', 'installer'))
            if not skip: lines += ([''] if lines else []) + ['**%s**' % section]
            continue
        if skip or not t.startswith(('- ', '* ')): continue
        if sum(len(l) + 1 for l in lines) > 2800:
            if count >= 0: lines.append('…and more in the full patch notes.'); count = -1
            continue
        lines.append('• ' + shorten(t[2:]))
    lines += ['', '**[⬇ Download the installer](%s)** · [Full patch notes and known issues](%s)' % (INSTALLER_URL, page_url),
              '', '*Already installed? Open the installer and press Update.*']
    return {'embeds': [{'title': title, 'url': page_url, 'description': '\n'.join(lines)[:4000], 'color': GOLD}]}

def webhook_url():
    path = os.environ.get('BWVR_DISCORD_WEBHOOK_FILE') or os.path.join(os.path.expanduser('~'), '.bwvr', 'discord_webhook.txt')
    try: return open(path, encoding='utf-8').read().strip() or None
    except OSError: return None

def post_discord(card):
    url = webhook_url()
    if not url: print('discord: no webhook file, no post'); return
    if url.endswith('/github'): url = url[:-len('/github')]
    req = urllib.request.Request(url, data=json.dumps(card).encode('utf-8'), method='POST',
                                 headers={'Content-Type': 'application/json', 'User-Agent': 'bwvr-release-script'})
    try:
        with urllib.request.urlopen(req) as r: print('discord: posted (%d)' % r.status)
    except Exception as e: print('discord: not posted:', e)  # the release itself is done

body = open(notes_file, encoding='utf-8').read()
if preview:
    card = discord_card(body, 'https://github.com/%s/releases/tag/%s' % (repo, tag))
    print(json.dumps(card, indent=2))
    print('webhook file:', 'found' if webhook_url() else 'missing')
    sys.exit(0)

out = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\n\n',
                     capture_output=True, text=True, check=True).stdout
token = dict(l.split('=', 1) for l in out.splitlines() if '=' in l).get('password')
if not token: sys.exit('no stored github.com credential')

def call(method, url, data=None, ctype='application/json', ok404=False):
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header('Authorization', 'Bearer ' + token)
    req.add_header('Accept', 'application/vnd.github+json')
    req.add_header('User-Agent', 'bwvr-release-script')
    if data is not None: req.add_header('Content-Type', ctype)
    try:
        with urllib.request.urlopen(req) as r: return json.loads(r.read() or b'null')
    except urllib.error.HTTPError as e:
        if ok404 and e.code == 404: return None
        sys.exit('%s %s -> %d %s' % (method, url.split('?')[0], e.code, e.read()[:300]))

api = 'https://api.github.com/repos/' + repo
print('signed in as', call('GET', 'https://api.github.com/user')['login'])
rel = call('GET', api + '/releases/tags/' + tag, ok404=True)
created = rel is None
if rel is None:
    rel = call('POST', api + '/releases', json.dumps({'tag_name': tag, 'target_commitish': 'main', 'name': title,
                                                      'body': body, 'draft': False, 'prerelease': False}).encode())
    print('created', rel['html_url'])
else:
    old_assets = rel.get('assets', [])
    rel = call('PATCH', api + '/releases/%d' % rel['id'], json.dumps({'name': title, 'body': body}).encode())
    rel['assets'] = old_assets
    print('updated', rel['html_url'])
names = [os.path.basename(a) for a in assets]
for old in rel.get('assets', []):
    if old['name'] in names or old['name'] in remove:
        call('DELETE', api + '/releases/assets/%d' % old['id'])
        print('removed', old['name'])
upload = rel['upload_url'].split('{')[0]
for a in assets:
    name = os.path.basename(a)
    ctype = 'application/zip' if name.endswith('.zip') else 'text/plain' if name.endswith('.sha256') else 'application/octet-stream'
    r = call('POST', upload + '?name=' + urllib.request.quote(name), open(a, 'rb').read(), ctype)
    print('asset', r['name'], r['size'], r.get('digest'))

# Only one installer download on the releases page: when this release carries the installer, it is
# taken off every other release (with any copy still named the old way, BWVR-Setup-<version>.exe).
# Installers look across all releases for the newest one, so updating works the same.
INSTALLER = ('JakeypooBWVRModInstaller.exe', 'installer.json')
if any(n in INSTALLER for n in names):
    for other in call('GET', api + '/releases?per_page=100'):
        if other['id'] == rel['id']: continue
        for a in other.get('assets', []):
            if a['name'] in INSTALLER or (a['name'].startswith('BWVR-Setup-') and a['name'].endswith('.exe')):
                call('DELETE', api + '/releases/assets/%d' % a['id'])
                print('removed', a['name'], 'from', other['tag_name'])

# Announce a new release once (not when an existing one is updated, e.g. a fixed installer).
if created and not no_discord: post_discord(discord_card(body, rel['html_url']))
