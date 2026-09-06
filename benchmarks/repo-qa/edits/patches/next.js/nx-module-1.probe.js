// nx-module-1.probe.js is the discrimination probe for edit task nx-module-1
// (ts-named-reexport-barrel): the barrel module
// packages/next/src/build/webpack/config/blocks/css/loaders/index.ts must
// re-export its public members through explicit named export blocks, never a
// wildcard star re-export (`export * from` / `export * as ns from`).
//
// The probe is a TypeScript-compiler-API assertion over the materialized
// tree (cwd); it never matches source text with a regex.
// Exit 0: every re-export in the barrel is a named `export { ... } from`
// block and at least one named re-export exists. Exit 1: otherwise.
'use strict';

const { execSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function loadTypeScript() {
  const candidates = [execSync('npm root -g').toString().trim()];
  const fnmRoot = path.join(os.homedir(), '.local/share/fnm/node-versions');
  if (fs.existsSync(fnmRoot)) {
    for (const version of fs.readdirSync(fnmRoot)) {
      candidates.push(
        path.join(fnmRoot, version, 'installation/lib/node_modules')
      );
    }
  }
  for (const dir of candidates) {
    if (fs.existsSync(path.join(dir, 'typescript/package.json'))) {
      module.paths.push(dir);
      return require('typescript');
    }
  }
  console.error(
    'probe: cannot resolve the typescript package from any global install ' +
      'root: ' + candidates.join(', ')
  );
  process.exit(1);
}

const ts = loadTypeScript();

const TARGET_FILE =
  'packages/next/src/build/webpack/config/blocks/css/loaders/index.ts';

function main() {
  let text;
  try {
    text = fs.readFileSync(TARGET_FILE, 'utf8');
  } catch (err) {
    console.error(`probe: cannot read ${TARGET_FILE}: ${err.message}`);
    process.exit(1);
  }
  const sf = ts.createSourceFile(
    TARGET_FILE,
    text,
    ts.ScriptTarget.Latest,
    true,
    ts.ScriptKind.TS
  );
  if (sf.parseDiagnostics.length > 0) {
    console.error(`probe: ${TARGET_FILE} does not parse cleanly`);
    process.exit(1);
  }

  let wildcard = null;
  let namedReexports = 0;
  for (const st of sf.statements) {
    if (!ts.isExportDeclaration(st) || !st.moduleSpecifier) continue;
    if (!st.exportClause || ts.isNamespaceExport(st.exportClause)) {
      const line = sf.getLineAndCharacterOfPosition(st.getStart()).line + 1;
      wildcard = `line ${line} (${st.moduleSpecifier.text})`;
    } else if (
      ts.isNamedExports(st.exportClause) &&
      st.exportClause.elements.length > 0
    ) {
      namedReexports++;
    }
  }

  if (wildcard) {
    console.error(
      `probe: ${TARGET_FILE} re-exports through a wildcard at ${wildcard} ` +
        'instead of an explicit named export block'
    );
    process.exit(1);
  }
  if (namedReexports > 0) {
    process.exit(0);
  }
  console.error(
    `probe: no named re-export block found in ${TARGET_FILE}`
  );
  process.exit(1);
}

main();
