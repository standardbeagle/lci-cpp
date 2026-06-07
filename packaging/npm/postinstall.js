#!/usr/bin/env node
// Downloads the prebuilt lci binary matching this package version + the host
// platform from GitHub releases, and stages it in vendor/. Fails fast on any
// unsupported platform, network, or extraction error — no silent fallback.
//
// Contract: see
// docs/superpowers/specs/2026-06-07-install-update-distribution-design.md

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const https = require('https');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const REPO = 'standardbeagle/lci-cpp';
const version = require('./package.json').version;

function fail(msg) {
  console.error(`lci install error: ${msg}`);
  process.exit(1);
}

// Resolve the platform -> asset matcher, mirroring the shared contract.
function assetMatcher() {
  const platform = process.platform;
  const arch = process.arch;
  if (platform === 'linux') {
    if (arch !== 'x64') {
      fail(`unsupported architecture for Linux: ${arch} (only x64 has a release artifact)`);
    }
    return (name) => name.endsWith('Linux.tar.gz');
  }
  if (platform === 'darwin') {
    // Universal binary serves both x64 and arm64.
    return (name) => name.endsWith('Darwin.tar.gz');
  }
  if (platform === 'win32') {
    if (arch !== 'x64') {
      fail(`unsupported architecture for Windows: ${arch} (only x64 has a release artifact)`);
    }
    return (name) => /\.tar\.gz$/.test(name) && /win64|windows/i.test(name);
  }
  fail(`unsupported platform: ${platform}`);
  return null;
}

const ALLOWED_HOSTS = new Set([
  'github.com',
  'api.github.com',
  'codeload.github.com',
  'githubusercontent.com',
]);

function isAllowedUrl(url) {
  let u;
  try {
    u = new URL(url);
  } catch (e) {
    return false;
  }
  if (u.protocol !== 'https:') return false;
  return ALLOWED_HOSTS.has(u.hostname) || u.hostname.endsWith('.githubusercontent.com');
}

// HTTPS GET following redirects (GitHub hosts only); resolves to a Buffer.
function get(url) {
  return new Promise((resolve, reject) => {
    if (!isAllowedUrl(url)) {
      reject(new Error(`refusing non-GitHub URL: ${url}`));
      return;
    }
    https
      .get(url, { headers: { 'User-Agent': 'lci-npm-installer', Accept: '*/*' } }, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          res.resume();
          if (!isAllowedUrl(res.headers.location)) {
            reject(new Error(`refusing redirect to non-GitHub host: ${res.headers.location}`));
            return;
          }
          resolve(get(res.headers.location));
          return;
        }
        if (res.statusCode !== 200) {
          reject(new Error(`HTTP ${res.statusCode} for ${url}`));
          return;
        }
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => resolve(Buffer.concat(chunks)));
      })
      .on('error', reject);
  });
}

async function main() {
  const match = assetMatcher();
  const api = `https://api.github.com/repos/${REPO}/releases/tags/v${version}`;

  let release;
  try {
    release = JSON.parse((await get(api)).toString('utf8'));
  } catch (e) {
    fail(`failed to query release v${version}: ${e.message}`);
  }

  const asset = (release.assets || []).find((a) => match(a.name));
  if (!asset) {
    fail(`no release asset for this platform in v${version}`);
  }

  const tarCheck = spawnSync('tar', ['--version'], { stdio: 'ignore' });
  if (tarCheck.error || tarCheck.status !== 0) {
    fail('tar is required but was not found on PATH');
  }

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lci-npm-'));
  const tarball = path.join(tmp, asset.name);
  let data;
  try {
    data = await get(asset.browser_download_url);
  } catch (e) {
    fail(`download failed: ${e.message}`);
  }

  // Verify integrity against the release SHA256SUMS when present.
  const sums = (release.assets || []).find((a) => a.name === 'SHA256SUMS');
  if (sums) {
    let sumsText;
    try {
      sumsText = (await get(sums.browser_download_url)).toString('utf8');
    } catch (e) {
      fail(`failed to fetch SHA256SUMS: ${e.message}`);
    }
    const line = sumsText.split('\n').find((l) => l.trim().endsWith(asset.name));
    if (!line) fail(`SHA256SUMS has no entry for ${asset.name}`);
    const expected = line.trim().split(/\s+/)[0].toLowerCase();
    const actual = crypto.createHash('sha256').update(data).digest('hex');
    if (actual !== expected) {
      fail(`checksum mismatch for ${asset.name} (expected ${expected}, got ${actual})`);
    }
  } else {
    console.warn('lci: release has no SHA256SUMS; skipping integrity check');
  }

  fs.writeFileSync(tarball, data);

  const extract = spawnSync('tar', ['-xzf', tarball, '-C', tmp], { stdio: 'inherit' });
  if (extract.status !== 0) {
    fail('failed to extract archive');
  }

  const binName = process.platform === 'win32' ? 'lci.exe' : 'lci';
  const found = findFile(tmp, binName);
  if (!found) {
    fail('extracted archive did not contain the lci binary');
  }

  const vendor = path.join(__dirname, 'vendor');
  fs.mkdirSync(vendor, { recursive: true });
  const dest = path.join(vendor, binName);
  fs.copyFileSync(found, dest);
  if (process.platform !== 'win32') {
    fs.chmodSync(dest, 0o755);
  }
  fs.rmSync(tmp, { recursive: true, force: true });
  console.log(`lci ${version} installed (${process.platform}/${process.arch}).`);
}

function findFile(root, name) {
  const stack = [root];
  while (stack.length) {
    const dir = stack.pop();
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) stack.push(full);
      else if (entry.name === name) return full;
    }
  }
  return null;
}

main().catch((e) => fail(e.message));
