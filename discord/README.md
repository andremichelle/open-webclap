# Posting plugins to the WebCLAPs Discord channel

In git, except the webhook URL: `discord/.webhook` holds it alone (one line, git-ignored, readable only by you).
One post per bundle in the cloud catalogue: its screenshot, what it is, its
description, license and links. The scripts here made the first 33 posts (2026-11-10).

## A post

```
## <bundle name>
WebCLAP <instrument | audio effect | note effect> by <the catalogue folder: the original author>

<the catalogue's description>

**License** <license>
**Original** <credits url>
**Source** <source url, a commit pinned as /tree/main/>
```

- A bundle with several plugins (Signalsmith Basics): "N WebCLAP audio effects by ...", a "Plugins:" line, and a
  collage of all of them as the image.
- The text comes from the published index (`https://assets.opendaw.studio/wclaps/index.json`), so fix a description
  in the admin app first (admin.opendaw.studio, Edit on the bundle, Publish), then post.
- Links in `<...>` so Discord shows no preview, only the screenshot. Mentions are off.

## Screenshots

Needs the site's dev server (`cd site && npm run dev`, port 8130) and Google Chrome. 2x device scale, PNG.

- **A plugin with a page on the site**: `node discord/site.mjs http://localhost:8130 build/screenshots <id>...`
  takes the plugin's window (the larger frame), or its device panel where there is no window. `<id>#panel` takes
  the device panel instead. `<id>?local` loads the build in `build/` rather than the cloud's bundle (use it when the
  page cannot fetch the cloud bundle, and to shoot a build before it is uploaded).
- **A bundle without a site page** (another author's, e.g. Tapa, Pro54): download it from the cloud,
  `curl -o x.wclap.tar.gz https://assets.opendaw.studio/wclaps/<uuid>.wclap`, then
  `node discord/import.mjs http://localhost:8130 build/screenshots <name>=<path to the .wclap.tar.gz>` (the site's
  Import button, headless).
- **OpenRB's panels with made-up signals**: `node discord/shoot.mjs http://localhost:8130/openrb-preview/index.html?preview build/openrb/screenshots`
  (the preview page, untracked in `site/public/openrb-preview/`).
- **A collage** (several plugins in one bundle): put the shots side by side on openDAW's background
  (hsl(197, 8%, 9%)), each with its name above in small grey; see the Python (PIL) used for Signalsmith Basics and
  OpenRB in the session of 2026-11-10: rows of images, 24 px padding, 20 px gaps, 2x.

Look at every image before posting: an empty page or a loading screen means the plugin was not ready.

## Posting

```sh
curl -s -o /tmp/index.json "https://assets.opendaw.studio/wclaps/index.json?t=$(date +%s)"
python3 -I discord/post.py /tmp/index.json build/screenshots discord/posted.json <bundle uuid>=<image.png> ...
```

- The webhook URL is in the channel's settings (Integrations, Webhooks). It lives in `discord/.webhook` alone
  (`printf '%s\n' '<url>' > discord/.webhook && chmod 600 discord/.webhook`), or set `WEBHOOK` for one run. Never
  put it in any other file, a commit or a command that is saved.
- `discord/posted.json` keeps each bundle's message id. A bundle already in it is **edited in place** (text and image
  replaced, same message); a new one is posted. If a message was deleted in Discord, the edit answers 404: remove
  its uuid from `posted.json` and run again to post it anew.
- Two seconds between posts. Post in the catalogue's order (the index's folders top to bottom) when doing many.
- The bundle uuids: in the admin app (Copy UUID) or the index.

## After a bundle is replaced in the cloud

The replacement has a new uuid. Edit the old post rather than posting again: in `posted.json`, rename the old uuid's
key to the new one (the message id stays), retake the screenshot, then run `post.py` with the new uuid.
