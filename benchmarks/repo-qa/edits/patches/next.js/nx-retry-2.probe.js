// nx-retry-2.probe.js is the discrimination probe for edit task nx-retry-2
// (ts-throw-typed-error): in fileExistsInDirectory
// (packages/next/src/build/webpack/loaders/next-app-loader/index.ts) the
// unexpected state - the loader running without an active webpack
// compilation, a state the code comments say should never be hit - must
// throw a typed Error (an identifier whose name ends in `Error`), never
// swallow the state by returning a false sentinel from a catch clause.
//
// The probe is a TypeScript-compiler-API assertion over the materialized
// tree (cwd); it never matches source text with a regex.
// Exit 0: no catch clause in the target function returns a false sentinel
// and the function throws a typed error on the unexpected state.
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

const TARGET_FILE =
  'packages/next/src/build/webpack/loaders/next-app-loader/index.ts';
const TARGET_FUNC = 'fileExistsInDirectory';

function isTypedErrorThrow(node) {
  return (
    ts.isThrowStatement(node) &&
    node.expression &&
    ts.isNewExpression(node.expression) &&
    ts.isIdentifier(node.expression.expression) &&
    node.expression.expression.text.endsWith('Error')
  );
}

function findTargetFunction(sf) {
  let found = null;
  (function visit(node) {
    if (found) return;
    if (
      (ts.isArrowFunction(node) || ts.isFunctionExpression(node)) &&
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

  const fn = findTargetFunction(sf);
  if (!fn) {
    console.error(`probe: ${TARGET_FUNC} not found in ${TARGET_FILE}`);
    process.exit(1);
  }

  let sentinelCatch = null;
  let typedThrow = false;
  (function visit(node) {
    if (ts.isCatchClause(node)) {
      let sentinel = false;
      (function walkCatch(inner) {
        if (
          ts.isReturnStatement(inner) &&
          inner.expression &&
          inner.expression.kind === ts.SyntaxKind.FalseKeyword
        ) {
          sentinel = true;
        }
        ts.forEachChild(inner, walkCatch);
      })(node.block);
      if (sentinel) {
        sentinelCatch =
          sf.getLineAndCharacterOfPosition(node.getStart()).line + 1;
      }
    }
    if (isTypedErrorThrow(node)) typedThrow = true;
    ts.forEachChild(node, visit);
  })(fn);

  if (sentinelCatch) {
    console.error(
      `probe: the unexpected-state catch at ${TARGET_FILE}:${sentinelCatch} ` +
        `in ${TARGET_FUNC} swallows the state by returning a false sentinel ` +
        'instead of throwing a typed Error'
    );
    process.exit(1);
  }
  if (typedThrow) {
    process.exit(0);
  }
  console.error(
    `probe: no typed-error throw found on the unexpected state of ` +
      `${TARGET_FUNC} (${TARGET_FILE})`
  );
  process.exit(1);
}

main();
