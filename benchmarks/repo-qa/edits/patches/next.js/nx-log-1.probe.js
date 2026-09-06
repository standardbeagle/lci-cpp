// nx-log-1.probe.js is the discrimination probe for edit task nx-log-1
// (ts-central-log-namespace): the build diagnostic inside getParentOutput in
// packages/next/src/build/adapter/build-complete.ts must go through the
// shared Log namespace imported from the central build/output/log module -
// never a raw console.* call.
//
// The probe is a TypeScript-compiler-API assertion over the materialized
// tree (cwd); it never matches source text with a regex. The Log import is
// recognised in BOTH specifier forms (bare `next/dist/build/output/log` and
// relative `.../output/log`) per bench-harness-oracle-independence rule 3.
// Exit 0: the target function contains no console.* call and at least one
// call through the Log namespace alias. Exit 1: otherwise.
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

const TARGET_FILE = 'packages/next/src/build/adapter/build-complete.ts';
const TARGET_FUNC = 'getParentOutput';

// A specifier names the central log module when it is the bare first-party
// alias `next/dist/build/output/log` or any relative path ending in
// `output/log`.
function isCentralLogSpecifier(specifier) {
  return (
    specifier === 'next/dist/build/output/log' ||
    ((specifier.startsWith('./') || specifier.startsWith('../')) &&
      specifier.endsWith('/output/log'))
  );
}

function findTargetFunction(sf) {
  let found = null;
  (function visit(node) {
    if (found) return;
    if (
      (ts.isFunctionDeclaration(node) ||
        ts.isFunctionExpression(node) ||
        ts.isArrowFunction(node)) &&
      node.parent &&
      ts.isVariableDeclaration(node.parent) &&
      ts.isIdentifier(node.parent.name) &&
      node.parent.name.text === TARGET_FUNC
    ) {
      found = node;
      return;
    }
    if (
      ts.isFunctionDeclaration(node) &&
      node.name &&
      node.name.text === TARGET_FUNC
    ) {
      found = node;
      return;
    }
    ts.forEachChild(node, visit);
  })(sf);
  return found;
}

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

  // Resolve the local alias of the central Log namespace import.
  let logAlias = null;
  for (const st of sf.statements) {
    if (
      ts.isImportDeclaration(st) &&
      ts.isStringLiteral(st.moduleSpecifier) &&
      isCentralLogSpecifier(st.moduleSpecifier.text) &&
      st.importClause &&
      st.importClause.namedBindings &&
      ts.isNamespaceImport(st.importClause.namedBindings)
    ) {
      logAlias = st.importClause.namedBindings.name.text;
    }
  }

  const fn = findTargetFunction(sf);
  if (!fn) {
    console.error(`probe: ${TARGET_FUNC} not found in ${TARGET_FILE}`);
    process.exit(1);
  }

  let consoleCall = null;
  let logCall = false;
  (function visit(node) {
    if (
      ts.isCallExpression(node) &&
      ts.isPropertyAccessExpression(node.expression) &&
      ts.isIdentifier(node.expression.expression)
    ) {
      const receiver = node.expression.expression.text;
      if (receiver === 'console' && !consoleCall) {
        consoleCall = 'console.' + node.expression.name.text;
      }
      if (logAlias && receiver === logAlias) {
        logCall = true;
      }
    }
    ts.forEachChild(node, visit);
  })(fn);

  if (consoleCall) {
    console.error(
      `probe: ${TARGET_FUNC} in ${TARGET_FILE} emits a build diagnostic via ` +
        `${consoleCall} instead of the shared Log namespace`
    );
    process.exit(1);
  }
  if (logCall) {
    process.exit(0);
  }
  console.error(
    `probe: no shared Log namespace diagnostic found in ${TARGET_FUNC} ` +
      `(${TARGET_FILE})`
  );
  process.exit(1);
}

main();
