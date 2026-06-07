#!/usr/bin/env node
// Thin launcher: exec the vendored lci binary staged by postinstall.js,
// passing through argv, stdio, and exit code.
'use strict';

const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');

const binName = process.platform === 'win32' ? 'lci.exe' : 'lci';
const bin = path.join(__dirname, '..', 'vendor', binName);

if (!fs.existsSync(bin)) {
  console.error(
    'lci: vendored binary missing. Reinstall the package (npm rebuild @standardbeagle/lci) to fetch it.'
  );
  process.exit(1);
}

const result = spawnSync(bin, process.argv.slice(2), { stdio: 'inherit' });
if (result.error) {
  console.error(`lci: ${result.error.message}`);
  process.exit(1);
}
process.exit(result.status === null ? 1 : result.status);
