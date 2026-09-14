"""Writes dist/artifact.html: index.html as a Claude Artifact publish takes it.

The Artifact tool wraps a page in its own doctype, head (charset and viewport) and body, so the page it is given
must not carry them. This keeps the title and stylesheet links from <head>, then the body's content. Publish
dist/artifact.html as the page, with every module, detent/ and the stylesheets mapped at the artifact's root.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
page = (root / 'index.html').read_text(encoding='utf-8')
head = re.search(r'<head[^>]*>(.*?)</head>', page, re.S | re.I)
body = re.search(r'<body[^>]*>(.*)</body>', page, re.S | re.I)
if not head or not body:
    sys.exit('index.html needs a <head> and a <body>')

keep = [line for line in head.group(1).strip().split('\n')
        if line.strip() and not re.match(r'\s*<meta\s+(charset|name="viewport")', line, re.I)]
out = root / 'dist' / 'artifact.html'
out.parent.mkdir(exist_ok=True)
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(keep) + '\n\n' + body.group(1).strip('\n') + '\n')
print(f'wrote {out.relative_to(root).as_posix()}')
