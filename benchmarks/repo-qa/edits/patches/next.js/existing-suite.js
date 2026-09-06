// existing-suite.js is the shared existing-suite substitute for the next.js
// edit tasks (nx-api-*, nx-log-*, nx-module-*, nx-retry-*).
//
// The forged next.js tree has pnpm-lock.yaml but no node_modules, and the
// bank's declared suite (`pnpm -w test`) is the e2e browser suite: installing
// (~2GB plus a Rust/turbopack build) cannot fit DEFAULT_TIMEOUT=120. This
// substitute is the strongest whole-corpus check that runs hermetically: it
// parses every .ts/.tsx under packages/next/src/lib and packages/next/src/build
// (the blast radius of every nx-* task) with the TypeScript parser and fails
// on any syntactic diagnostic. It is SYNTAX-ONLY by design (declared in each
// task's existing_suite.notes); it never type-checks and never executes code.
//
// Runs with cwd = the materialized tree. Exit 0: every file parses clean.
// Exit 1: at least one file carries a parse diagnostic.
'use strict';

const { execSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

// The tree has no node_modules; resolve the TypeScript compiler API from the
// host's global installs (run_command passes no env, so NODE_PATH is not an
// option).
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
    'existing-suite: cannot resolve the typescript package from any global ' +
      'install root: ' +
      candidates.join(', ')
  );
  process.exit(1);
}

const ts = loadTypeScript();

const ROOTS = ['packages/next/src/lib', 'packages/next/src/build'];

function* walk(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walk(p);
    } else if (/\.tsx?$/.test(entry.name)) {
      yield p;
    }
  }
}

function main() {
  const started = Date.now();
  for (const root of ROOTS) {
    if (!fs.existsSync(root)) {
      console.error(
        `existing-suite: required root ${root} is missing (cwd must be the ` +
          'materialized next.js tree)'
      );
      process.exit(1);
    }
  }
  let parsed = 0;
  const failures = [];
  for (const root of ROOTS) {
    for (const file of walk(root)) {
      const text = fs.readFileSync(file, 'utf8');
      const sf = ts.createSourceFile(
        file,
        text,
        ts.ScriptTarget.Latest,
        false,
        file.endsWith('.tsx') ? ts.ScriptKind.TSX : ts.ScriptKind.TS
      );
      parsed++;
      for (const diag of sf.parseDiagnostics) {
        const pos = sf.getLineAndCharacterOfPosition(diag.start);
        failures.push(
          `${file}:${pos.line + 1}:${pos.character + 1} - ` +
            ts.flattenDiagnosticMessageText(diag.messageText, '\n')
        );
      }
    }
  }
  const wall = ((Date.now() - started) / 1000).toFixed(1);
  if (failures.length > 0) {
    console.error(
      `existing-suite: ${failures.length} syntactic diagnostic(s) across ` +
        `${parsed} files under ${ROOTS.join(', ')}:`
    );
    for (const failure of failures.slice(0, 20)) {
      console.error('  ' + failure);
    }
    process.exit(1);
  }
  console.log(
    `existing-suite: ${parsed} .ts/.tsx files under ${ROOTS.join(', ')} ` +
      `parse clean (${wall}s)`
  );
  process.exit(0);
}

main();
