"""Posts catalogue bundles to a Discord webhook, one message each with its screenshot.
Usage: post.py <index.json> <screenshots dir> <posted.json> <uuid=image.png>...
The webhook URL comes from discord/.webhook (one line, git-ignored, never committed) or the WEBHOOK environment
variable; posted.json records each bundle's message id for edits."""
import json, os, re, sys, time, urllib.request, uuid as uuidlib

index_path, shots, posted_path, *pairs = sys.argv[1:]
# the channel's webhook URL: discord/.webhook beside this script (git-ignored), else $WEBHOOK
WEBHOOK_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".webhook")
webhook = os.environ.get("WEBHOOK") or (open(WEBHOOK_FILE).read().strip() if os.path.exists(WEBHOOK_FILE) else "")
if not webhook.startswith("https://discord.com/api/webhooks/"):
    sys.exit(f"no webhook URL: put it in {WEBHOOK_FILE} (one line) or set WEBHOOK")
index = json.load(open(index_path))
entries = {}
def walk(folder):
    for w in folder.get("wclaps", []): entries[w["uuid"]] = (folder["name"], w)
    for f in folder.get("folders", []): walk(f)
for f in index["folders"]: walk(f)
posted = json.load(open(posted_path)) if os.path.exists(posted_path) else {}

def kind_of(features):
    if "instrument" in features: return "instrument"
    if "note-effect" in features: return "note effect"
    return "audio effect"

def text_of(vendor, entry):
    plugins = entry["plugins"]
    kinds = {kind_of(p["features"]) for p in plugins}
    if len(plugins) > 1:
        what = f"{len(plugins)} WebCLAP {'audio effects' if kinds == {'audio effect'} else 'plugins'}"
    else:
        what = f"WebCLAP {kind_of(plugins[0]['features'])}"
    lines = [f"## {entry['name']}", f"{what} by {vendor}", "", entry.get("description", "")]
    if len(plugins) > 1:
        lines += ["", "Plugins: " + ", ".join(p["name"] for p in plugins)]
    lines += [""]
    lines += [f"**License** {entry['license']}"]
    if entry.get("credits"): lines += [f"**Original** <{entry['credits']}>"]
    # a source pinned to a commit points at main instead
    source = re.sub(r"/tree/[0-9a-f]{40}/", "/tree/main/", entry.get("url", ""))
    if source and source != entry.get("credits"): lines += [f"**Source** <{source}>"]
    return "\n".join(lines)

def multipart(fields, file_name, file_bytes):
    boundary = uuidlib.uuid4().hex
    body = b""
    for name, value in fields.items():
        body += f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\nContent-Type: application/json\r\n\r\n".encode()
        body += value.encode() + b"\r\n"
    body += f"--{boundary}\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"{file_name}\"\r\nContent-Type: image/png\r\n\r\n".encode()
    body += file_bytes + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"

for pair in pairs:
    uuid, image = pair.split("=")
    vendor, entry = entries[uuid]
    content = text_of(vendor, entry)
    payload = {"content": content, "allowed_mentions": {"parse": []},
               "attachments": [{"id": 0, "filename": image}]}
    body, ctype = multipart({"payload_json": json.dumps(payload)}, image, open(os.path.join(shots, image), "rb").read())
    if uuid in posted:
        request = urllib.request.Request(f"{webhook}/messages/{posted[uuid]}", data=body, method="PATCH",
                                         headers={"Content-Type": ctype, "User-Agent": "webclap-catalogue-post"})
    else:
        request = urllib.request.Request(f"{webhook}?wait=true", data=body, method="POST",
                                         headers={"Content-Type": ctype, "User-Agent": "webclap-catalogue-post"})
    with urllib.request.urlopen(request) as response:
        message = json.load(response)
    posted[uuid] = message["id"]
    json.dump(posted, open(posted_path, "w"), indent=1)
    print(f"{entry['name']}: message {message['id']} ({len(content)} chars)")
    time.sleep(2)
