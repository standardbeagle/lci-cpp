// nx-api-2.probe.js is the discrimination probe for edit task nx-api-2
// (ts-exported-function-declaration): the helper scheduleOnNextTick in
// packages/next/src/lib/scheduler.ts must be exposed as an
// exported function declaration with explicit parameters - not a const
// holding an arrow/function that is exported directly or through a late
// export block.
//
// The probe is a TypeScript-compiler-API assertion over the materialized
// tree (cwd); it never matches source text with a regex.
// Exit 0: an exported FunctionDeclaration named scheduleOnNextTick exists
// and no variable binding or bare named-export block re-exports that name.
// Exit 1: otherwise.
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

const TARGET_FILE = 'packages/next/src/lib/scheduler.ts';
const TARGET_NAME = 'scheduleOnNextTick';

const hasExportModifier = (modifiers) =>
  modifiers && modifiers.some((m) => m.kind === ts.SyntaxKind.ExportKeyword);

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

  let exportedFunction = false;
  let offPattern = null;

  for (const st of sf.statements) {
    if (
      ts.isFunctionDeclaration(st) &&
      st.name &&
      st.name.text === TARGET_NAME &&
      hasExportModifier(st.modifiers)
    ) {
      exportedFunction = true;
    }
    if (ts.isVariableStatement(st)) {
      for (const decl of st.declarationList.declarations) {
        if (ts.isIdentifier(decl.name) && decl.name.text === TARGET_NAME) {
          offPattern = `a const binding (${TARGET_NAME} = ...)`;
        }
      }
    }
    if (
      ts.isExportDeclaration(st) &&
      !st.moduleSpecifier &&
      st.exportClause &&
      ts.isNamedExports(st.exportClause) &&
      st.exportClause.elements.some(
        (el) => (el.propertyName || el.name).text === TARGET_NAME
      )
    ) {
      offPattern = 'a late `export { ... }` block';
    }
  }

  if (offPattern) {
    console.error(
      `probe: ${TARGET_NAME} in ${TARGET_FILE} is exported through ` +
        `${offPattern} instead of an exported function declaration`
    );
    process.exit(1);
  }
  if (exportedFunction) {
    process.exit(0);
  }
  console.error(
    `probe: no exported function declaration named ${TARGET_NAME} found in ` +
      TARGET_FILE
  );
  process.exit(1);
}

main();
