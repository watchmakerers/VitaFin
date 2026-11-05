import http.server, json, io, re, sys, hashlib, colorsys
from urllib.parse import urlparse, parse_qs
from PIL import Image, ImageDraw, ImageFont
F = "/usr/share/fonts/noto/NotoSans-Bold.ttf"
MOVIES = ["Moonrise Harbor","The Last Lantern","Glass Orchard","Northbound","Paper Satellites","Ember Valley","Quiet Machines","Saltwater Saints","The Cartographer","Blue Hour","Iron Meadow","Little Comets","Atlas of Rain","Hollow Crown Road","Neon Tide","Winter Signal","The Ninth Gate Hotel","Copper Sky"]
def item(i, name, typ="Movie", **kw):
    d = {"Id": "m%02d" % i, "Name": name, "Type": typ, "ProductionYear": 2008 + i % 15, "RunTimeTicks": (88 + i * 3) * 600000000,
         "ImageTags": {"Primary": "p%d" % i}, "BackdropImageTags": ["b%d" % i], "IsFolder": False,
         "UserData": {"Played": i % 5 == 1, "PlaybackPositionTicks": 0 if i % 4 else 21000000000}, "CommunityRating": 6.1 + (i % 30) / 10,
         "OfficialRating": "PG-13", "Overview": "A quiet drifter returns to the coastal town she left behind and finds that the lighthouse keeper has been sending letters to someone who isn't there. Over one long winter, old friendships are tested and a secret buried under the harbour comes to light."}
    d.update(kw); return d
VIEWS = [{"Id": "lib1", "Name": "Movies", "Type": "CollectionFolder", "CollectionType": "movies", "ImageTags": {"Primary": "v1"}},
         {"Id": "lib2", "Name": "TV Shows", "Type": "CollectionFolder", "CollectionType": "tvshows", "ImageTags": {"Primary": "v2"}},
         {"Id": "lib3", "Name": "Music", "Type": "CollectionFolder", "CollectionType": "music", "ImageTags": {"Primary": "v3"}},
         {"Id": "lib4", "Name": "Home Videos", "Type": "CollectionFolder", "CollectionType": "homevideos", "ImageTags": {}}]
def poster(name, w, h):
    hsh = int(hashlib.md5(name.encode()).hexdigest()[:6], 16)
    hue = (hsh % 360) / 360
    im = Image.new("RGB", (w, h)); px = im.load()
    for y in range(h):
        r, g, b = colorsys.hsv_to_rgb(hue + y / h * 0.12, 0.65, 0.95 - 0.6 * y / h)
        for x in range(w): px[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    d = ImageDraw.Draw(im)
    d.ellipse([w * 0.2, h * 0.18, w * 0.8, h * 0.18 + w * 0.6], fill=tuple(int(c * 255) for c in colorsys.hsv_to_rgb(hue + 0.5, 0.4, 1)))
    f = ImageFont.truetype(F, max(10, w // 9))
    words = name.split(); lines = []; cur = ""
    for wd in words:
        if d.textlength(cur + " " + wd, font=f) > w * 0.85 and cur: lines.append(cur); cur = wd
        else: cur = (cur + " " + wd).strip()
    lines.append(cur)
    y = h * 0.66
    for ln in lines: d.text(((w - d.textlength(ln, font=f)) / 2, y), ln, font=f, fill=(255, 255, 255)); y += w // 8
    b = io.BytesIO(); im.save(b, "JPEG", quality=85); return b.getvalue()
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, body, ctype="application/json"):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        self.send_response(200); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); self.rfile.read(n)
        if self.path.startswith("/Users/AuthenticateByName"):
            return self.send({"AccessToken": "tok", "User": {"Id": "u", "Name": "nathan"}})
        self.send({})
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path
        if p == "/System/Info/Public": return self.send({"Id": "x", "ServerName": "Living Room Server"})
        if p.endswith("/Views"): return self.send({"Items": VIEWS, "TotalRecordCount": len(VIEWS)})
        if p.endswith("/Items/Resume"): return self.send({"Items": [item(3, MOVIES[3])], "TotalRecordCount": 1})
        if p == "/Shows/NextUp": return self.send({"Items": [item(40, "Pilot", "Episode", SeriesName="Harbor Lights", ParentIndexNumber=1, IndexNumber=2)], "TotalRecordCount": 1})
        m = re.match(r"/Items/(\w+)/Images/(\w+)", p)
        if m:
            iid = m.group(1); w = int(q.get("fillWidth", ["200"])[0]); h = int(q.get("fillHeight", ["300"])[0])
            name = next((x for i, x in enumerate(MOVIES) if "m%02d" % i == iid), {"lib1": "MOVIES", "lib2": "TV SHOWS", "lib3": "MUSIC"}.get(iid, iid))
            return self.send(poster(name, min(w, 480), min(h, 480)), "image/jpeg")
        m = re.match(r"/Users/\w+/Items/(\w+)$", p)
        if m:
            i = int(m.group(1)[1:]) if m.group(1)[1:].isdigit() else 0
            return self.send(item(i, MOVIES[i % len(MOVIES)]))
        if p.endswith("/Items"): return self.send({"Items": [item(i, n) for i, n in enumerate(MOVIES)], "TotalRecordCount": len(MOVIES)})
        self.send({"Items": [], "TotalRecordCount": 0})
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
