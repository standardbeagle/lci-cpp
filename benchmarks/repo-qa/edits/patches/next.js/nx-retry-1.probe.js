// nx-retry-1.probe.js is the discrimination probe for edit task nx-retry-1
// (ts-throw-typed-error): in getPackageVersion
// (packages/next/src/lib/get-package-version.ts) the unexpected-state branch
// - the dependency is declared in package.json yet its package.json cannot
// be resolved or read - must throw a typed Error (an identifier whose name
// ends in `Error`), never swallow the failure by returning a null sentinel.
//
// The probe is a TypeScript-compiler-API assertion over the materialized
// tree (cwd); it never matches source text with a regex.
// Exit 0: the target function's catch clause throws a typed error and no
// catch clause returns a null sentinel. Exit 1: otherwise.
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

const TARGET_FILE = 'packages/next/src/lib/get-package-version.ts';
const TARGET_FUNC = 'getPackageVersion';

function isTypedErrorThrow(node) {
  return (
    ts.isThrowStatement(node) &&
    node.expression &&
    ts.isNewExpression(node.expression) &&
    ts.isIdentifier(node.expression.expression) &&
    node.expression.expression.text.endsWith('Error')
  );
}

function isSentinelReturn(node, sentinelKind) {
  return (
    ts.isReturnStatement(node) &&
    node.expression &&
    node.expression.kind === sentinelKind
  );
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

  const fn = sf.statements.find(
    (st) =>
      ts.isFunctionDeclaration(st) && st.name && st.name.text === TARGET_FUNC
  );
  if (!fn) {
    console.error(`probe: ${TARGET_FUNC} not found in ${TARGET_FILE}`);
    process.exit(1);
  }

  let sentinelCatch = null;
  let typedThrow = false;
  (function visit(node) {
    if (ts.isCatchClause(node)) {
      let sentinel = false;
      let threw = false;
      (function walkCatch(inner) {
        if (isSentinelReturn(inner, ts.SyntaxKind.NullKeyword)) sentinel = true;
        if (isTypedErrorThrow(inner)) threw = true;
        ts.forEachChild(inner, walkCatch);
      })(node.block);
      if (sentinel && !threw) {
        sentinelCatch =
          sf.getLineAndCharacterOfPosition(node.getStart()).line + 1;
      }
      if (threw) typedThrow = true;
    }
    ts.forEachChild(node, visit);
  })(fn);

  if (sentinelCatch) {
    console.error(
      `probe: the unexpected-state catch at ${TARGET_FILE}:${sentinelCatch} ` +
        'in getPackageVersion swallows the failure by returning a null ' +
        'sentinel instead of throwing a typed Error'
    );
    process.exit(1);
  }
  if (typedThrow) {
    process.exit(0);
  }
  console.error(
    `probe: no typed-error throw found in the unexpected-state branch of ` +
      `${TARGET_FUNC} (${TARGET_FILE})`
  );
  process.exit(1);
}

main();
