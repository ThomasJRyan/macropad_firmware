import { gzipSync } from "node:zlib";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const root = new URL(".", import.meta.url).pathname;
const dist = join(root, "dist");

function minifyCss(source) {
  return source
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\s+/g, " ")
    .replace(/\s*([{}:;,>+~])\s*/g, "$1")
    .replace(/;}/g, "}")
    .replace(/\b0(px|em|rem|%)\b/g, "0")
    .trim();
}

function needsJsSpace(left, right) {
  return /[A-Za-z0-9_$]/.test(left) && /[A-Za-z0-9_$]/.test(right);
}

function minifyJs(source) {
  let output = "";
  let quote = "";
  let escaped = false;
  let pendingSpace = false;

  for (const char of source) {
    if (quote) {
      output += char;
      if (escaped) {
        escaped = false;
      } else if (char === "\\") {
        escaped = true;
      } else if (char === quote) {
        quote = "";
      }
      continue;
    }

    if (char === "'" || char === '"' || char === "`") {
      if (pendingSpace && output && needsJsSpace(output.at(-1), char)) output += " ";
      pendingSpace = false;
      quote = char;
      output += char;
      continue;
    }

    if (/\s/.test(char)) {
      pendingSpace = true;
      continue;
    }

    if (pendingSpace && output && needsJsSpace(output.at(-1), char)) output += " ";
    pendingSpace = false;
    output += char;
  }

  return output.trim();
}

function minifyHtml(source) {
  return source
    .replace(/<script>([\s\S]*?)<\/script>/g, (_, script) => `<script>${minifyJs(script)}</script>`)
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/>\s+</g, "><")
    .replace(/\s+/g, " ")
    .replace(/\s+(?=>)/g, "")
    .trim();
}

function writeMinified(name, content) {
  const path = join(dist, name);
  writeFileSync(path, content);
  writeFileSync(`${path}.gz`, gzipSync(content, { level: 9 }));
}

mkdirSync(dist, { recursive: true });

const css = minifyCss(readFileSync(join(root, "styles.css"), "utf8"));
const index = minifyHtml(readFileSync(join(root, "index.html"), "utf8"));
const wifi = minifyHtml(readFileSync(join(root, "wifi_setup.html"), "utf8"));

writeMinified("styles.css", css);
writeMinified("index.html", index);
writeMinified("wifi_setup.html", wifi);

for (const file of ["index.html", "wifi_setup.html", "styles.css"]) {
  const raw = readFileSync(join(root, file)).byteLength;
  const min = readFileSync(join(dist, file)).byteLength;
  const gz = readFileSync(join(dist, `${file}.gz`)).byteLength;
  const saved = raw - min;
  const pct = Math.round((saved / raw) * 100);
  console.log(`${file}: ${raw} -> ${min} bytes (${pct}% smaller), gzip ${gz} bytes`);
}
