# tmp_ui

Static staging copies of the firmware HTML UI.

Serve from this directory root:

```sh
python3 -m http.server 8080 -d tmp_ui
```

Then open:

- `http://localhost:8080/` for the station-mode button config page.
- `http://localhost:8080/wifi_setup.html` for the setup AP Wi-Fi page.

The `api/` files are static mock responses for visual iteration.

Generate compact firmware assets:

```sh
node tmp_ui/minify.mjs
```

The minified files are written to `tmp_ui/dist/`. Gzip variants are also
generated; those are the smallest option if the firmware serves them with a
`Content-Encoding: gzip` response header.
