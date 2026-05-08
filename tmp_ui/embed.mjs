import { readFileSync, writeFileSync } from "node:fs";

const assets = [
  ["UI_INDEX_HTML", "tmp_ui/index.html"],
  ["UI_WIFI_SETUP_HTML", "tmp_ui/wifi_setup.html"],
  ["UI_STYLES_CSS", "tmp_ui/styles.css"],
];

function assetArray(name, file) {
  const bytes = [...readFileSync(file), 0];
  const lines = [];

  for (let index = 0; index < bytes.length; index += 12) {
    lines.push(
      `    ${bytes
        .slice(index, index + 12)
        .map((byte) => `0x${byte.toString(16).padStart(2, "0")}`)
        .join(", ")}`,
    );
  }

  return `static const char ${name}[] = {\n${lines.join(",\n")}\n};\nstatic const size_t ${name}_LEN = sizeof(${name}) - 1u;\n`;
}

let output = "";
output += "#ifndef MACROPAD_UI_ASSETS_H\n";
output += "#define MACROPAD_UI_ASSETS_H\n\n";
output += "#include <stddef.h>\n\n";
output += "/* Generated from tmp_ui readable assets. */\n";

for (const [name, file] of assets) {
  output += `\n${assetArray(name, file)}`;
}

output += "\n#endif\n";

writeFileSync("src/ui_assets.h", output);
