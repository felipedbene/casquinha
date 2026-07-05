#!/usr/bin/env python3
# Edit the app icon with Nano Banana (Gemini image models). Reads the API key
# from /tmp/nano.key (never hardcoded).
# Usage: python3 tools/nano_edit.py IN OUT "prompt" [model]
# Default model is Nano Banana Pro (gemini-3-pro-image); falls back to
# gemini-2.5-flash-image if the Pro model errors.
import sys, json, base64, urllib.request

KEYFILE = "/tmp/nano.key"
inp   = sys.argv[1] if len(sys.argv) > 1 else "design/icon/base-from-degelato.png"
outp  = sys.argv[2] if len(sys.argv) > 2 else "design/icon/casquinha-icon.png"
prompt = sys.argv[3] if len(sys.argv) > 3 else (
    "Edit this app icon into a sibling for a different app. Keep the EXACT same "
    "art style: the same chunky cartoon otter with sleek over-ear headphones, the "
    "same warm brown fur and linework, and the same glossy rounded-square "
    "app-icon frame. Change TWO things: (1) replace the three-scoop gelato with a "
    "single-scoop plain vanilla ICE-CREAM CONE (a 'casquinha' — one round scoop on "
    "a golden waffle cone) held in the otter's paws; (2) swap the purple aurora "
    "background for a warm cream-and-orange late-1990s glow, and tuck a tiny classic "
    "beige Macintosh computer with a smiling Happy-Mac face on its screen into the "
    "lower background as a retro Mac OS 9 nod. Keep it a clean, centered, square "
    "app icon.")
models = [sys.argv[4]] if len(sys.argv) > 4 else ["gemini-3-pro-image", "gemini-2.5-flash-image"]

key = open(KEYFILE).read().strip()
b64 = base64.b64encode(open(inp, "rb").read()).decode()
body = {"contents": [{"parts": [
    {"text": prompt},
    {"inline_data": {"mime_type": "image/png", "data": b64}},
]}]}

last_err = None
for model in models:
    url = "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent" % model
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
        headers={"x-goog-api-key": key, "Content-Type": "application/json"})
    try:
        resp = json.load(urllib.request.urlopen(req, timeout=180))
    except Exception as e:
        last_err = "%s: %s" % (model, e)
        detail = getattr(e, "read", None)
        if detail:
            try: last_err += " | " + detail().decode()[:300]
            except Exception: pass
        print("model %s failed, trying next…" % model)
        continue

    got = None
    for cand in resp.get("candidates", []):
        for part in cand.get("content", {}).get("parts", []):
            d = part.get("inline_data") or part.get("inlineData")
            if d and d.get("data"):
                got = d["data"]; break
        if got: break
    if got:
        open(outp, "wb").write(base64.b64decode(got))
        print("wrote %s (via %s)" % (outp, model))
        sys.exit(0)
    last_err = "%s: no image in response: %s" % (model, json.dumps(resp)[:400])
    print("model %s returned no image, trying next…" % model)

print("FAILED:", last_err); sys.exit(1)
