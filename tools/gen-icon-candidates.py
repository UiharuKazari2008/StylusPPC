"""Generate app icon candidates with the OpenAI Images API.

The API key is read from the OPENAI_API_KEY environment variable (never stored or printed).
Usage (PowerShell):
    $env:OPENAI_API_KEY = "sk-..."      # in your own terminal only
    python tools/gen-icon-candidates.py [count-per-style]
Writes PNGs to icon-candidates/ (git-ignored).
"""
import base64
import json
import os
import sys
import urllib.request

API = "https://api.openai.com/v1/images/generations"
MODELS = "https://api.openai.com/v1/models"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "icon-candidates")

BASE = ("App icon for 'PocketStylus', an Android app that runs classic Windows Mobile / Pocket PC "
        "phones. Square 1:1, centered subject with generous padding, bold simple shapes that read at "
        "48 px, no text, no letters, no Microsoft logo, no Windows flag. ")
STYLES = {
    "glass-stylus": BASE + "A glossy 2009-era glass Pocket PC phone seen at a slight angle with a silver "
    "stylus leaning across it; Vista/Aero glass highlights, deep blue and teal, soft reflection, dark "
    "gradient background.",
    "stylus-mark": BASE + "A minimal flat mark: a stylus tip touching a rounded phone screen, with a small "
    "tap ripple; two-tone deep navy and electric cyan, modern Material You style.",
    "retro-pda": BASE + "A chunky late-2000s PDA phone with a slide-out keyboard, rendered as a polished "
    "3D toy-like object; warm orange and graphite, studio lighting, plain light background.",
    "titanium": BASE + "Inspired by the Windows Mobile 6.5 'Titanium' home screen: stacked glossy "
    "horizontal bars on a dark phone screen with a stylus; black, silver and a single vivid accent "
    "colour, sleek and premium.",
    "pixel": BASE + "A pixel-art Pocket PC with a stylus, crisp 32x32-style pixels scaled up, limited "
    "palette of blues and greys, nostalgic.",
}


def latest_image_model(key):
    """The newest image model this key can use (the gpt-image family, newest created first)."""
    req = urllib.request.Request(MODELS, headers={"Authorization": "Bearer " + key})
    with urllib.request.urlopen(req, timeout=60) as r:
        models = json.load(r)["data"]
    images = [m for m in models if m["id"].startswith("gpt-image") and "mini" not in m["id"]]
    if not images:
        images = [m for m in models if m["id"].startswith(("gpt-image", "dall-e"))]
    if not images:
        sys.exit("No image model available to this key.")
    return max(images, key=lambda m: m.get("created", 0))["id"]


def generate(prompt, key, model):
    body = json.dumps({"model": model, "prompt": prompt, "size": "1024x1024", "n": 1}).encode()
    req = urllib.request.Request(API, data=body, headers={
        "Authorization": "Bearer " + key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        data = json.load(r)
    return base64.b64decode(data["data"][0]["b64_json"])


def main():
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if not key:
        sys.exit("Set OPENAI_API_KEY in this terminal first.")
    per = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    os.makedirs(OUT, exist_ok=True)
    model = latest_image_model(key)
    print("model:", model)
    for name, prompt in STYLES.items():
        for i in range(per):
            path = os.path.join(OUT, f"{name}-{i + 1}.png")
            try:
                with open(path, "wb") as f:
                    f.write(generate(prompt, key, model))
                print("wrote", path)
            except Exception as e:  # keep going with the other styles
                print("failed", name, i + 1, e)


if __name__ == "__main__":
    main()
