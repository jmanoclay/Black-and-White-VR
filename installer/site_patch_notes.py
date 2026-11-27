# Writes the website's patch notes page from the release notes (installer/notes/<version>.md),
# newest first, each with its release date from GitHub and, for some versions, pictures of what's new.
# Usage: python -I installer\site_patch_notes.py <out.html> [--date <version>=<YYYY-MM-DD> ...]
#   e.g. python -I installer\site_patch_notes.py D:\Git\BW-VR-Releases\docs\patch-notes\index.html
# --date gives a version's date before its release exists (the page is made just before publishing).
# Read-only on GitHub (the public releases list, no sign-in).
import html, json, os, re, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ID = 1442480971  # jmanoclay/Black-and-White-VR, by its permanent id
SITE = 'https://jmanoclay.github.io/Black-and-White-VR/'

# Pictures shown with a version's notes: (file in docs/img/, what it shows).
PICTURES = {
    '0.4.0': [
        ('wrist.jpg', 'Wrist controls over the back of the hand, Villager info pressed with a fingertip'),
        ('villager-info.jpg', "A villager's info on a label over them, upright in the world"),
        ('teleport-creature.jpg', 'The teleport line turns pale blue on your creature'),
        ('highlight.jpg', 'What your hand will grab lights up: the rock under the hand glows, the one beside it stays grey'),
    ],
}

args = sys.argv[1:]
if not args: sys.exit(__doc__ or 'usage: site_patch_notes.py <out.html> [--date v=YYYY-MM-DD ...]')
out = args[0]
dates = {}
i = 1
while i < len(args):
    if args[i] == '--date' and i + 1 < len(args):
        v, d = args[i + 1].split('=', 1); dates[v] = d; i += 2
    else: sys.exit('unknown argument: ' + args[i])

req = urllib.request.Request('https://api.github.com/repositories/%d/releases?per_page=100' % REPO_ID,
                             headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'bwvr-site'})
with urllib.request.urlopen(req) as r:
    for rel in json.loads(r.read()):
        v = rel['tag_name'].lstrip('v')
        if not rel.get('draft') and v not in dates: dates[v] = (rel.get('published_at') or '')[:10]

def key(v): return tuple(int(x) for x in re.findall(r'\d+', v))
versions = sorted((f[:-3] for f in os.listdir(os.path.join(HERE, 'notes')) if f.endswith('.md')), key=key, reverse=True)
versions = [v for v in versions if v in dates]  # only released ones (or given a date)

def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', t)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'\[([^\]]+)\]\((https?://[^)\s]+)\)', r'<a href="\2">\1</a>', t)
    return t

def render(md):
    out, depth = [], 0  # depth: open <ul> levels
    for raw in md.replace('\r', '').split('\n'):
        if not raw.strip(): continue
        m = re.match(r'^(\s*)[-*] (.*)$', raw)
        if m:
            level = len(m.group(1)) // 2 + 1
            while depth < level: out.append('<ul>'); depth += 1
            while depth > level: out.append('</ul>'); depth -= 1
            out.append('<li>%s</li>' % inline(m.group(2)))
            continue
        while depth: out.append('</ul>'); depth -= 1
        h = re.match(r'^(#+)\s*(.*)$', raw.strip())
        if h: out.append('<h3>%s</h3>' % inline(h.group(2)))
        else: out.append('<p>%s</p>' % inline(raw.strip()))
    while depth: out.append('</ul>'); depth -= 1
    return '\n'.join(out)

def nice(d):
    months = 'January February March April May June July August September October November December'.split()
    y, m, dd = d.split('-'); return '%s %d, %s' % (months[int(m) - 1], int(dd), y)

items = []
for n, v in enumerate(versions):
    md = open(os.path.join(HERE, 'notes', v + '.md'), encoding='utf-8').read()
    pics = ''
    if v in PICTURES:
        pics = '<div class="pics">' + ''.join(
            '<figure><img src="../img/%s" alt="%s" loading="lazy" width="960" height="540"><figcaption>%s</figcaption></figure>'
            % (f, html.escape(a), html.escape(a)) for f, a in PICTURES[v]) + '</div>'
    items.append('''<article class="release%s" id="v%s">
  <header><h2>Version %s</h2><time datetime="%s">%s</time>%s</header>
  %s
  <div class="notes">
%s
  </div>
</article>''' % (' latest' if n == 0 else '', v, v, dates[v], nice(dates[v]),
                 ' <span class="tag">Latest</span>' if n == 0 else '', pics, render(md)))

nav = ''.join('<a href="#v%s">%s</a>' % (v, v) for v in versions)
page = open(os.path.join(HERE, 'site_patch_notes.html'), encoding='utf-8').read()
page = page.replace('{{RELEASES}}', '\n\n'.join(items)).replace('{{NAV}}', nav).replace('{{LATEST}}', versions[0])
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, 'w', encoding='utf-8', newline='\n') as f: f.write(page)
print('wrote %s: %d versions, newest %s' % (out, len(versions), versions[0]))
